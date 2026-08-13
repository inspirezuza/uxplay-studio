#pragma once

#include "airplayworker.h"
#include "appstate.h"
#include "receiverconfig.h"

#include <QObject>
#include <QPointer>
#include <functional>

class QImage;

class ReceiverEngine final : public QObject {
    Q_OBJECT

public:
    using WorkerFactory = std::function<AirPlayWorker *()>;

    explicit ReceiverEngine(QObject *parent = nullptr);
    ReceiverEngine(WorkerFactory workerFactory, int shutdownWaitMs,
                   QObject *parent = nullptr);
    ~ReceiverEngine() override;

    ReceiverState state() const;
    bool isRunning() const;
    void start(const ReceiverConfig &config, quintptr videoWindow, const QString &bleStatusPath);
    void stop();
    void restart(const ReceiverConfig &config, quintptr videoWindow, const QString &bleStatusPath);

signals:
    void stateChanged(ReceiverState state);
    void eventReceived(const ReceiverEvent &event);
    void previewFrame(const QImage &frame);
    void recoveryScheduled(int delayMs);

private:
    void launchWorker();
    void handleEvent(const ReceiverEvent &event);
    void handleWorkerExit(int exitCode);
    void handleWorkerFinished();
    void transitionTo(ReceiverState next);

    QPointer<AirPlayWorker> m_worker;
    WorkerFactory m_workerFactory;
    int m_shutdownWaitMs = 3000;
    ReceiverStateMachine m_stateMachine;
    ReceiverConfig m_config;
    quintptr m_videoWindow = 0;
    QString m_bleStatusPath;
    bool m_userStopping = false;
    bool m_restartRequested = false;
    int m_retryAttempt = 0;
};
