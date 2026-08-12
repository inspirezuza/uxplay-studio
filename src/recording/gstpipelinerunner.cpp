#include "gstpipelinerunner.h"

#include <gst/gst.h>

#include <QElapsedTimer>

GstPipelineRunner::GstPipelineRunner() {
    GError *error = nullptr;
    if (!gst_is_initialized()) gst_init_check(nullptr, nullptr, &error);
    if (error) g_error_free(error);
}

GstPipelineRunner::~GstPipelineRunner() {
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
    for (const Track &track : m_tracks) gst_element_send_event(track.pipeline, gst_event_new_eos());
    for (const Track &track : m_tracks) {
        GstBus *bus = gst_element_get_bus(track.pipeline);
        const int remaining = qMax(0, timeoutMs - static_cast<int>(elapsed.elapsed()));
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, static_cast<GstClockTime>(remaining) * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (!message || GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            clean = false;
            if (error && error->isEmpty()) *error = QStringLiteral("%1 did not finalize cleanly").arg(track.name);
        }
        if (message) gst_message_unref(message);
        gst_object_unref(bus);
        gst_element_set_state(track.pipeline, GST_STATE_NULL);
        gst_object_unref(track.pipeline);
    }
    m_tracks.clear();
    return clean;
}

bool GstPipelineRunner::updateVideoCapture(const QRect &captureRect, QString *error) {
    for (const Track &track : m_tracks) {
        if (track.name != QStringLiteral("airplay-video")) continue;
        GstElement *source = gst_bin_get_by_name(GST_BIN(track.pipeline), "studio-capture");
        if (!source) {
            if (error) *error = QStringLiteral("The AirPlay capture source is unavailable");
            return false;
        }
        g_object_set(source,
                     "crop-x", static_cast<guint>(qMax(0, captureRect.x())),
                     "crop-y", static_cast<guint>(qMax(0, captureRect.y())),
                     "crop-width", static_cast<guint>(captureRect.width()),
                     "crop-height", static_cast<guint>(captureRect.height()), nullptr);
        gst_object_unref(source);
        return true;
    }
    if (error) *error = QStringLiteral("The AirPlay video track is not running");
    return false;
}
