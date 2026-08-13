#pragma once

#include "recordingsession.h"

#include <QList>
#include <QMutex>
#include <QString>

#include <gst/app/gstappsink.h>

typedef struct _GstElement GstElement;

class GstPipelineRunner final : public PipelineRunner {
public:
    GstPipelineRunner();
    ~GstPipelineRunner() override;
    bool startTrack(const QString &name, const QString &pipeline, QString *error) override;
    bool stopAll(int timeoutMs, QString *error) override;
    void setCameraPreviewCallback(std::function<void(const QImage &)> callback) override;
    void setTrackFirstMediaCallback(
        std::function<void(const QString &, qint64)> callback) override;

private:
    static GstFlowReturn pullCameraSample(GstAppSink *sink, gpointer context);
    static GstPadProbeReturn observeFirstMedia(GstPad *pad, GstPadProbeInfo *info,
                                               gpointer context);
    struct FirstMediaProbe {
        GstPipelineRunner *runner = nullptr;
        QString track;
    };
    struct Track { QString name; GstElement *pipeline = nullptr; };
    QList<Track> m_tracks;
    QMutex m_previewMutex;
    std::function<void(const QImage &)> m_cameraPreviewCallback;
    QMutex m_firstMediaMutex;
    std::function<void(const QString &, qint64)> m_firstMediaCallback;
};
