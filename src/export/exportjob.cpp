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
#include <functional>

namespace {
struct TimelineMetadata {
    qint64 durationNanoseconds = 0;
    QSize airplayVideoSize;
    QSize cameraVideoSize;
};

TimelineMetadata discoverTimelineMetadata(const ProjectInfo &project,
                                          const std::function<bool()> &cancelled) {
    TimelineMetadata metadata;
    QHash<QString, qint64> trackDurations;
    for (const QString &directory : {project.airplayDirectory(), project.presenterDirectory()}) {
        if (cancelled()) return {};
        QDirIterator files(directory, {QStringLiteral("*.mkv"), QStringLiteral("*.mka")}, QDir::Files);
        while (files.hasNext()) {
            if (cancelled()) return {};
            const QString path = files.next();
            GstDiscoverer *discoverer = gst_discoverer_new(3 * GST_SECOND, nullptr);
            if (!discoverer) continue;
            const QByteArray uri = QUrl::fromLocalFile(path).toEncoded();
            GError *error = nullptr;
            GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
            if (info) {
                QString prefix = QFileInfo(path).completeBaseName();
                prefix.remove(QRegularExpression(QStringLiteral("-\\d+$")));
                QString trackKey = prefix;
                if (directory == project.airplayDirectory()) {
                    trackKey = prefix == QStringLiteral("video")
                        ? QStringLiteral("airplay-video") : QStringLiteral("airplay-audio");
                }
                trackDurations[trackKey] += static_cast<qint64>(gst_discoverer_info_get_duration(info));
                GList *videoStreams = gst_discoverer_info_get_video_streams(info);
                if (videoStreams) {
                    const auto *video = GST_DISCOVERER_VIDEO_INFO(videoStreams->data);
                    const QSize size(static_cast<int>(gst_discoverer_video_info_get_width(video)),
                                     static_cast<int>(gst_discoverer_video_info_get_height(video)));
                    const QString prefix = QFileInfo(path).completeBaseName()
                        .remove(QRegularExpression(QStringLiteral("-\\d+$")));
                    if (size.isValid() && directory == project.airplayDirectory() &&
                        prefix == QStringLiteral("video") && !metadata.airplayVideoSize.isValid()) {
                        metadata.airplayVideoSize = size;
                    } else if (size.isValid() && directory == project.presenterDirectory() &&
                               prefix == QStringLiteral("camera") && !metadata.cameraVideoSize.isValid()) {
                        metadata.cameraVideoSize = size;
                    }
                    gst_discoverer_stream_info_list_free(videoStreams);
                }
                gst_discoverer_info_unref(info);
            }
            if (error) g_error_free(error);
            gst_object_unref(discoverer);
            if (cancelled()) return {};
        }
    }
    metadata.durationNanoseconds = ExportPipeline::alignedTimelineDuration(
        trackDurations, ExportPipeline::trackStartOffsets(project));
    return metadata;
}

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
        cairo_save(context);
        cairo_translate(context, width / 2.0, height / 2.0);
        cairo_scale(context, width / 2.0, height / 2.0);
        cairo_arc(context, 0, 0, 1.0, 0, 2 * 3.14159265358979323846);
        cairo_restore(context);
    } else {
        roundedRectangle(context, width, height, qMin(36.0, qMin(width, height) / 2.0));
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

bool cleanOrPreservePartial(const QString &temporaryOutput, QString *error) {
    if (!QFileInfo::exists(temporaryOutput) || QFile::remove(temporaryOutput)) return true;

    QString preserved = temporaryOutput + QStringLiteral(".incomplete");
    for (int suffix = 2; QFileInfo::exists(preserved); ++suffix) {
        preserved = temporaryOutput + QStringLiteral(".incomplete-%1").arg(suffix);
    }
    if (QFile::rename(temporaryOutput, preserved)) {
        if (error) {
            *error += QStringLiteral("; the unpublished partial output was preserved as %1")
                          .arg(QFileInfo(preserved).fileName());
        }
        return true;
    }
    if (error) {
        *error += QStringLiteral(
            "; the unpublished partial output could not be removed or preserved; "
            "the project remains Exporting so cleanup can be retried");
    }
    return false;
}
}

ExportJob::ExportJob(ProjectStore *store, QObject *parent) : QObject(parent), m_store(store) {}

ExportJob::~ExportJob() {
    if (m_thread && m_thread->isRunning()) {
        m_thread->requestInterruption();
        m_thread->wait();
    }
}

bool ExportJob::isRunning() const { return m_active.load(std::memory_order_acquire); }

bool ExportJob::start(const ProjectInfo &project, const SceneDocument &scene,
                      SceneFormat format, const QString &outputPath) {
    if (isRunning() || !m_store) return false;
    const ProjectLoadResult persisted = m_store->load(project.directory);
    if (!persisted.ok()) {
        emit failed(persisted.error);
        return false;
    }
    if (persisted.project.state != ProjectState::Ready) {
        emit failed(persisted.project.state == ProjectState::Recoverable
            ? QStringLiteral("Recover this project before export")
            : QStringLiteral("The project must be Ready before export"));
        return false;
    }
    const QString stateError = m_store->setState(project.directory, ProjectState::Exporting);
    if (!stateError.isEmpty()) {
        emit failed(stateError);
        return false;
    }
    emit started(outputPath);
    const QJsonObject sceneSnapshot = scene.toJson();
    m_thread = QThread::create([this, project, outputPath, sceneSnapshot, format]() {
        const QString temporaryOutput = outputPath + QStringLiteral(".partial");
        QFile::remove(temporaryOutput);
        const auto restoreReadyAfterFailure = [this, &project, &temporaryOutput](QString error) {
            const bool outputClean = cleanOrPreservePartial(temporaryOutput, &error);
            if (outputClean) {
                const QString stateError = m_store->setState(project.directory, ProjectState::Ready);
                if (!stateError.isEmpty()) {
                    error += QStringLiteral("; project state could not be restored: %1").arg(stateError);
                }
            }
            emit failed(error);
        };
        GError *initError = nullptr;
        if (!gst_is_initialized()) gst_init_check(nullptr, nullptr, &initError);
        if (initError) {
            const QString error = QString::fromUtf8(initError->message);
            g_error_free(initError);
            restoreReadyAfterFailure(error);
            return;
        }
        auto sceneResult = SceneDocument::fromJson(sceneSnapshot);
        if (!sceneResult.has_value()) {
            restoreReadyAfterFailure(sceneResult.error);
            return;
        }
        const auto cancelled = []() { return QThread::currentThread()->isInterruptionRequested(); };
        const TimelineMetadata timeline = discoverTimelineMetadata(project, cancelled);
        if (cancelled()) {
            m_store->setState(project.directory, ProjectState::Ready);
            return;
        }
        QHash<QString, QSize> sourcePixelSizes;
        for (const SceneSource &source : sceneResult.document->sources()) {
            if (source.type == SceneSourceType::AirPlay && timeline.airplayVideoSize.isValid())
                sourcePixelSizes.insert(source.id, timeline.airplayVideoSize);
            else if (source.type == SceneSourceType::Camera && timeline.cameraVideoSize.isValid())
                sourcePixelSizes.insert(source.id, timeline.cameraVideoSize);
        }
        const auto built = ExportPipeline::build(project, *sceneResult.document, format,
                                                  temporaryOutput, timeline.durationNanoseconds,
                                                  sourcePixelSizes);
        if (!built.ok()) {
            restoreReadyAfterFailure(built.error);
            return;
        }
        if (cancelled()) {
            m_store->setState(project.directory, ProjectState::Ready);
            return;
        }
        const QString pipelineDescription = built.description;
        GError *parseError = nullptr;
        GstElement *pipeline = gst_parse_launch(pipelineDescription.toUtf8().constData(), &parseError);
        if (!pipeline || parseError) {
            const QString error = parseError ? QString::fromUtf8(parseError->message)
                                             : QStringLiteral("Could not create export pipeline");
            if (parseError) g_error_free(parseError);
            if (pipeline) gst_object_unref(pipeline);
            restoreReadyAfterFailure(error);
            return;
        }
        GstBus *bus = gst_element_get_bus(pipeline);
        attachMasks(pipeline);
        if (cancelled()) {
            gst_object_unref(bus);
            gst_object_unref(pipeline);
            m_store->setState(project.directory, ProjectState::Ready);
            return;
        }
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
        if (success) {
            if (QFileInfo::exists(outputPath) || !QFile::rename(temporaryOutput, outputPath)) {
                restoreReadyAfterFailure(QStringLiteral(
                    "The completed export could not be moved into place"));
                return;
            }
            const QString stateError = m_store->setState(project.directory, ProjectState::Ready);
            if (stateError.isEmpty()) emit finished(outputPath);
            else restoreReadyAfterFailure(stateError);
        } else {
            restoreReadyAfterFailure(error.isEmpty()
                ? QStringLiteral("Export stopped before completion") : error);
        }
    });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    m_active.store(true, std::memory_order_release);
    connect(m_thread, &QThread::finished, this, [this]() {
        m_active.store(false, std::memory_order_release);
    }, Qt::DirectConnection);
    connect(m_thread, &QThread::finished, this, [this]() {
        m_thread = nullptr;
    });
    m_thread->start();
    return true;
}
