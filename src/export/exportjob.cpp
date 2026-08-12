#include "exportjob.h"

#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QHash>
#include <QThread>
#include <QRegularExpression>
#include <cairo.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <QUrl>
#include <gst/video/video.h>

namespace {
void roundedRectangle(cairo_t *context, double width, double height, double radius) {
    constexpr double pi = 3.14159265358979323846;
    cairo_new_sub_path(context);
    cairo_arc(context, width - radius, radius, radius, -pi / 2, 0);
    cairo_arc(context, width - radius, height - radius, radius, 0, pi / 2);
    cairo_arc(context, radius, height - radius, radius, pi / 2, pi);
    cairo_arc(context, radius, radius, radius, pi, 3 * pi / 2);
    cairo_close_path(context);
}

void drawMask(GstElement *overlay, cairo_t *context, guint64, guint64, gpointer data) {
    GstPad *pad = gst_element_get_static_pad(overlay, "sink");
    GstCaps *caps = gst_pad_get_current_caps(pad);
    GstVideoInfo info;
    const bool valid = caps && gst_video_info_from_caps(&info, caps);
    if (caps) gst_caps_unref(caps);
    gst_object_unref(pad);
    if (!valid) return;
    const double width = GST_VIDEO_INFO_WIDTH(&info);
    const double height = GST_VIDEO_INFO_HEIGHT(&info);
    cairo_save(context);
    cairo_set_operator(context, CAIRO_OPERATOR_CLEAR);
    cairo_set_fill_rule(context, CAIRO_FILL_RULE_EVEN_ODD);
    cairo_rectangle(context, 0, 0, width, height);
    if (GPOINTER_TO_INT(data) == static_cast<int>(SceneMask::Circle)) {
        const double radius = qMin(width, height) / 2.0;
        cairo_arc(context, width / 2.0, height / 2.0, radius, 0, 2 * 3.14159265358979323846);
    } else {
        roundedRectangle(context, width, height, qMin(width, height) * .09);
    }
    cairo_fill(context);
    cairo_restore(context);
}

void attachMasks(GstElement *pipeline) {
    for (int index = 0; index < 256; ++index) {
        for (const auto &entry : {qMakePair(QStringLiteral("circle-mask-%1").arg(index), SceneMask::Circle),
                                  qMakePair(QStringLiteral("rounded-mask-%1").arg(index), SceneMask::RoundedRectangle)}) {
            GstElement *overlay = gst_bin_get_by_name(GST_BIN(pipeline), entry.first.toUtf8().constData());
            if (!overlay) continue;
            g_signal_connect(overlay, "draw", G_CALLBACK(drawMask), GINT_TO_POINTER(static_cast<int>(entry.second)));
            gst_object_unref(overlay);
        }
    }
}
}

ExportJob::ExportJob(ProjectStore *store, QObject *parent) : QObject(parent), m_store(store) {}

ExportJob::~ExportJob() {
    if (m_thread && m_thread->isRunning()) {
        m_thread->requestInterruption();
        m_thread->wait();
    }
}

bool ExportJob::isRunning() const { return m_thread && m_thread->isRunning(); }

bool ExportJob::start(const ProjectInfo &project, const SceneDocument &scene,
                      SceneFormat format, const QString &outputPath) {
    if (isRunning() || !m_store) return false;
    qint64 durationNanoseconds = 0;
    QHash<QString, qint64> trackDurations;
    for (const QString &directory : {project.airplayDirectory(), project.presenterDirectory()}) {
        QDirIterator files(directory, {QStringLiteral("*.mkv"), QStringLiteral("*.mka")}, QDir::Files);
        while (files.hasNext()) {
            const QString path = files.next();
            GstDiscoverer *discoverer = gst_discoverer_new(3 * GST_SECOND, nullptr);
            if (!discoverer) continue;
            const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
            GError *error = nullptr;
            GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
            if (info) {
                QString trackKey = QFileInfo(path).completeBaseName();
                trackKey.remove(QRegularExpression(QStringLiteral("-\\d+$")));
                trackKey = directory + QLatin1Char('/') + trackKey;
                trackDurations[trackKey] += static_cast<qint64>(gst_discoverer_info_get_duration(info));
                gst_discoverer_info_unref(info);
            }
            if (error) g_error_free(error);
            gst_object_unref(discoverer);
        }
    }
    for (qint64 duration : trackDurations) durationNanoseconds = qMax(durationNanoseconds, duration);
    const auto built = ExportPipeline::build(project, scene, format, outputPath, durationNanoseconds);
    if (!built.ok()) { emit failed(built.error); return false; }
    m_store->setState(project.directory, ProjectState::Exporting);
    emit started(outputPath);
    const QString pipelineDescription = built.description;
    m_thread = QThread::create([this, project, outputPath, pipelineDescription]() {
        GError *initError = nullptr;
        if (!gst_is_initialized()) gst_init_check(nullptr, nullptr, &initError);
        if (initError) {
            const QString error = QString::fromUtf8(initError->message);
            g_error_free(initError);
            m_store->setState(project.directory, ProjectState::Failed);
            emit failed(error);
            return;
        }
        GError *parseError = nullptr;
        GstElement *pipeline = gst_parse_launch(pipelineDescription.toUtf8().constData(), &parseError);
        if (!pipeline || parseError) {
            const QString error = parseError ? QString::fromUtf8(parseError->message)
                                             : QStringLiteral("Could not create export pipeline");
            if (parseError) g_error_free(parseError);
            if (pipeline) gst_object_unref(pipeline);
            m_store->setState(project.directory, ProjectState::Failed);
            emit failed(error);
            return;
        }
        GstBus *bus = gst_element_get_bus(pipeline);
        attachMasks(pipeline);
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
        GstMessage *message = nullptr;
        while (!QThread::currentThread()->isInterruptionRequested() && !message) {
            message = gst_bus_timed_pop_filtered(
                bus, 200 * GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        }
        if (QThread::currentThread()->isInterruptionRequested() && !message)
            gst_element_send_event(pipeline, gst_event_new_eos());
        bool success = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
        QString error;
        if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError *gstError = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(message, &gstError, &debug);
            error = gstError ? QString::fromUtf8(gstError->message) : QStringLiteral("Export failed");
            if (gstError) g_error_free(gstError);
            g_free(debug);
        }
        if (message) gst_message_unref(message);
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(pipeline);
        m_store->setState(project.directory, success ? ProjectState::Ready : ProjectState::Failed);
        if (success) emit finished(outputPath);
        else emit failed(error.isEmpty() ? QStringLiteral("Export stopped before completion") : error);
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, [this]() { m_thread = nullptr; });
    m_thread->start();
    return true;
}
