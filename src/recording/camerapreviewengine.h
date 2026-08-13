#pragma once

#include <QImage>
#include <QObject>
#include <QTimer>

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
    void stoppedUnexpectedly(const QString &message);

private:
    static GstFlowReturn pullSample(GstAppSink *sink, gpointer context);
    void pollBus();

    GstElement *m_pipeline = nullptr;
    GstElement *m_sink = nullptr;
    QTimer m_busTimer;
};
