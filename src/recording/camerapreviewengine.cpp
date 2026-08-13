#include "camerapreviewengine.h"

#include <gst/video/video.h>

CameraPreviewEngine::CameraPreviewEngine(QObject *parent) : QObject(parent) {
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);
}

CameraPreviewEngine::~CameraPreviewEngine() {
    stop();
}

bool CameraPreviewEngine::start(QString *error, const QString &sourceOverride) {
    if (m_pipeline) return true;
    const QString source = sourceOverride.isEmpty()
        ? QStringLiteral("mfvideosrc do-timestamp=true") : sourceOverride;
    const QString description = source + QStringLiteral(
        " ! queue leaky=downstream max-size-buffers=2 ! videoconvert ! videoscale ! videorate "
        "! video/x-raw,format=BGRA,width=640,height=360,framerate=15/1 "
        "! appsink name=studio-camera-preview max-buffers=1 drop=true sync=false");

    GError *parseError = nullptr;
    m_pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!m_pipeline || parseError) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message)
                                      : QStringLiteral("The camera preview pipeline is unavailable");
        if (parseError) g_error_free(parseError);
        if (m_pipeline) gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        return false;
    }
    m_sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "studio-camera-preview");
    if (!m_sink) {
        if (error) *error = QStringLiteral("The camera preview output is unavailable");
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        return false;
    }
    GstAppSinkCallbacks callbacks{};
    callbacks.new_sample = &CameraPreviewEngine::pullSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(m_sink), &callbacks, this, nullptr);
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (error) *error = QStringLiteral("The camera could not start");
        stop();
        return false;
    }
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *startupError = gst_bus_timed_pop_filtered(bus, 300 * GST_MSECOND,
                                                          GST_MESSAGE_ERROR);
    if (startupError) {
        GError *gstError = nullptr;
        gchar *debug = nullptr;
        gst_message_parse_error(startupError, &gstError, &debug);
        if (error) *error = gstError ? QString::fromUtf8(gstError->message)
                                    : QStringLiteral("The camera could not start");
        if (gstError) g_error_free(gstError);
        g_free(debug);
        gst_message_unref(startupError);
        gst_object_unref(bus);
        stop();
        return false;
    }
    gst_object_unref(bus);
    return true;
}

void CameraPreviewEngine::stop() {
    if (!m_pipeline) return;
    if (m_sink) {
        GstAppSinkCallbacks callbacks{};
        gst_app_sink_set_callbacks(GST_APP_SINK(m_sink), &callbacks, nullptr, nullptr);
    }
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    if (m_sink) gst_object_unref(m_sink);
    gst_object_unref(m_pipeline);
    m_sink = nullptr;
    m_pipeline = nullptr;
}

bool CameraPreviewEngine::isRunning() const {
    return m_pipeline != nullptr;
}

GstFlowReturn CameraPreviewEngine::pullSample(GstAppSink *sink, gpointer context) {
    auto *engine = static_cast<CameraPreviewEngine *>(context);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!engine || !sample) return GST_FLOW_EOS;
    GstVideoInfo info;
    GstVideoFrame frame;
    if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
        gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
        const auto *pixels = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
        const QImage image(pixels, GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame),
                           GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_ARGB32);
        emit engine->frameReady(image.copy());
        gst_video_frame_unmap(&frame);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}
