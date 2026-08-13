#pragma once

#include <QThread>
#include <QStringList>
#include <QMetaType>

class QImage;

struct ReceiverEvent {
    int type = 0;
    QString deviceName;
    QString deviceModel;
    QString deviceId;
    QString message;
    int width = 0;
    int height = 0;
};

Q_DECLARE_METATYPE(ReceiverEvent)

class AirPlayWorker : public QThread {
    Q_OBJECT

public:
    explicit AirPlayWorker(QObject *parent = nullptr);
    void configure(QStringList args, quintptr videoWindow);
    virtual void stopAirplay();

signals:
    void engineStarted();
    void engineExited(int exitCode);
    void receiverEvent(const ReceiverEvent &event);
    void previewFrame(const QImage &frame);

protected:
    void run() override;

private:
    static void eventCallback(const struct uxplay_event *event, void *context);
    static void previewCallback(const unsigned char *data, int width, int height,
                                int stride, void *context);

    QStringList m_args;
    quintptr m_videoWindow = 0;
};
