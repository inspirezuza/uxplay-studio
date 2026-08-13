#include "gstpipelinerunner.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <QElapsedTimer>
#include <QImage>
#include <QMutexLocker>

GstPipelineRunner::GstPipelineRunner() {
    GError *error = nullptr;
    if (!gst_is_initialized()) gst_init_check(nullptr, nullptr, &error);
    if (error) g_error_free(error);
}

GstPipelineRunner::~GstPipelineRunner() {
    setTrackFirstMediaCallback({});
    QString ignored;
    stopAll(700, &ignored);
}

bool GstPipelineRunner::startTrack(const QString &name, const QString &description,
                                   QString *error) {
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!pipeline) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message)
                                      : QStringLiteral("Invalid GStreamer pipeline");
        if (parseError) g_error_free(parseError);
        return false;
    }
    if (parseError) g_error_free(parseError);
    GstElement *origin = gst_bin_get_by_name(GST_BIN(pipeline), "studio-track-origin");
    GstPad *originPad = origin ? gst_element_get_static_pad(origin, "src") : nullptr;
    if (!origin || !originPad) {
        if (error) *error = QStringLiteral("The %1 track has no timing observation point").arg(name);
        if (originPad) gst_object_unref(originPad);
        if (origin) gst_object_unref(origin);
        gst_object_unref(pipeline);
        return false;
    }
    auto *probe = new FirstMediaProbe{this, name};
    const gulong probeId = gst_pad_add_probe(
        originPad, GST_PAD_PROBE_TYPE_BUFFER, &GstPipelineRunner::observeFirstMedia,
        probe, [](gpointer data) { delete static_cast<FirstMediaProbe *>(data); });
    gst_object_unref(originPad);
    gst_object_unref(origin);
    if (probeId == 0) {
        delete probe;
        if (error) *error = QStringLiteral("Could not observe the %1 track timing").arg(name);
        gst_object_unref(pipeline);
        return false;
    }
    if (name == QStringLiteral("camera")) {
        GstElement *preview = gst_bin_get_by_name(GST_BIN(pipeline), "studio-camera-preview");
        if (preview) {
            GstAppSinkCallbacks callbacks{};
            callbacks.new_sample = &GstPipelineRunner::pullCameraSample;
            gst_app_sink_set_callbacks(GST_APP_SINK(preview), &callbacks, this, nullptr);
            gst_object_unref(preview);
        }
    }
    const GstStateChangeReturn result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (result == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = QStringLiteral("GStreamer rejected the %1 track").arg(name);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    GstState current = GST_STATE_NULL;
    const GstStateChangeReturn ready = gst_element_get_state(
        pipeline, &current, nullptr, 1200 * GST_MSECOND);
    GstBus *startupBus = gst_element_get_bus(pipeline);
    GstMessage *startupError = gst_bus_pop_filtered(startupBus, GST_MESSAGE_ERROR);
    if (ready == GST_STATE_CHANGE_FAILURE || startupError) {
        GstMessage *message = startupError;
        if (message) {
            GError *gstError = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &gstError, &debug);
            if (error) *error = gstError ? QString::fromUtf8(gstError->message)
                                         : QStringLiteral("The track could not start");
            if (gstError) g_error_free(gstError);
            g_free(debug);
            gst_message_unref(message);
        }
        gst_object_unref(startupBus);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }
    gst_object_unref(startupBus);
    m_tracks.append({name, pipeline});
    return true;
}

bool GstPipelineRunner::stopAll(int timeoutMs, QString *error) {
    bool clean = true;
    QElapsedTimer elapsed;
    elapsed.start();
    for (const Track &track : m_tracks) {
        if (!track.failed) gst_element_send_event(track.pipeline, gst_event_new_eos());
    }
    for (const Track &track : m_tracks) {
        GstMessage *message = nullptr;
        GstBus *bus = nullptr;
        if (!track.failed) {
            bus = gst_element_get_bus(track.pipeline);
            const int remaining = qMax(0, timeoutMs - static_cast<int>(elapsed.elapsed()));
            message = gst_bus_timed_pop_filtered(
                bus, static_cast<GstClockTime>(remaining) * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        }
        if (track.failed || !message || GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            clean = false;
            if (error && error->isEmpty()) {
                *error = track.failure.isEmpty()
                    ? QStringLiteral("%1 did not finalize cleanly").arg(track.name)
                    : track.failure;
            }
        }
        if (message) gst_message_unref(message);
        if (bus) gst_object_unref(bus);
        gst_element_set_state(track.pipeline, GST_STATE_NULL);
        gst_object_unref(track.pipeline);
    }
    m_tracks.clear();
    return clean;
}

QStringList GstPipelineRunner::takeRuntimeFailures() {
    QStringList failures;
    for (Track &track : m_tracks) {
        if (track.failed) continue;
        GstBus *bus = gst_element_get_bus(track.pipeline);
        GstMessage *message = gst_bus_pop_filtered(
            bus, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        gst_object_unref(bus);
        if (!message) continue;

        QString detail;
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *gstError = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &gstError, &debug);
            if (gstError) detail = QString::fromUtf8(gstError->message);
            if (gstError) g_error_free(gstError);
            g_free(debug);
        }
        gst_message_unref(message);
        track.failed = true;
        track.failure = detail.isEmpty()
            ? QStringLiteral("%1 track stopped unexpectedly").arg(track.name)
            : QStringLiteral("%1 track stopped unexpectedly: %2").arg(track.name, detail);
        failures.append(track.failure);
    }
    return failures;
}

void GstPipelineRunner::setCameraPreviewCallback(
    std::function<void(const QImage &)> callback) {
    QMutexLocker lock(&m_previewMutex);
    m_cameraPreviewCallback = std::move(callback);
}

void GstPipelineRunner::setTrackFirstMediaCallback(
    std::function<void(const QString &, qint64)> callback) {
    QMutexLocker lock(&m_firstMediaMutex);
    m_firstMediaCallback = std::move(callback);
}

GstPadProbeReturn GstPipelineRunner::observeFirstMedia(GstPad *, GstPadProbeInfo *info,
                                                       gpointer context) {
    auto *probe = static_cast<FirstMediaProbe *>(context);
    if (!probe || !probe->runner || !(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER))
        return GST_PAD_PROBE_OK;
    const qint64 observedAtNanoseconds = static_cast<qint64>(g_get_monotonic_time()) * 1000;
    {
        QMutexLocker lock(&probe->runner->m_firstMediaMutex);
        if (probe->runner->m_firstMediaCallback) {
            probe->runner->m_firstMediaCallback(probe->track, observedAtNanoseconds);
        }
    }
    return GST_PAD_PROBE_REMOVE;
}

GstFlowReturn GstPipelineRunner::pullCameraSample(GstAppSink *sink, gpointer context) {
    auto *runner = static_cast<GstPipelineRunner *>(context);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!runner || !sample) return GST_FLOW_EOS;
    QImage image;
    GstVideoInfo info;
    GstVideoFrame frame;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
        gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
        const auto *pixels = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        image = QImage(pixels, GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame),
                       GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_ARGB32).copy();
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample);
    std::function<void(const QImage &)> callback;
    {
        QMutexLocker lock(&runner->m_previewMutex);
        callback = runner->m_cameraPreviewCallback;
    }
    if (callback && !image.isNull()) callback(image);
    return GST_FLOW_OK;
}
