#pragma once

#include <QImage>
#include <QObject>

#include <gst/app/gstappsink.h>

class CameraPreviewEngine final : public QObject {
    Q_OBJECT

public:
    explicit CameraPreviewEngine(QObject *parent = nullptr);
    ~CameraPreviewEngine() override;

    bool start(QString *error = nullptr, const QString &sourceOverride = {});
    void stop();
    bool isRunning() const;

signals:
    void frameReady(const QImage &frame);

private:
    static GstFlowReturn pullSample(GstAppSink *sink, gpointer context);

    GstElement *m_pipeline = nullptr;
    GstElement *m_sink = nullptr;
};
