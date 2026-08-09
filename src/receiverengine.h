#pragma once

#include "airplayworker.h"
#include "appstate.h"
#include "receiverconfig.h"

#include <QObject>
#include <QPointer>

class ReceiverEngine final : public QObject {
    Q_OBJECT

public:
    explicit ReceiverEngine(QObject *parent = nullptr);
    ~ReceiverEngine() override;

    ReceiverState state() const;
    bool isRunning() const;
    void start(const ReceiverConfig &config, quintptr videoWindow, const QString &bleStatusPath);
    void stop();
    void restart(const ReceiverConfig &config, quintptr videoWindow, const QString &bleStatusPath);

signals:
    void stateChanged(ReceiverState state);
    void eventReceived(const ReceiverEvent &event);
    void recoveryScheduled(int delayMs);

private:
    void launchWorker();
    void handleEvent(const ReceiverEvent &event);
    void handleWorkerExit(int exitCode);
    void handleWorkerFinished();
    void transitionTo(ReceiverState next);

    QPointer<AirPlayWorker> m_worker;
    ReceiverStateMachine m_stateMachine;
    ReceiverConfig m_config;
    quintptr m_videoWindow = 0;
    QString m_bleStatusPath;
    bool m_userStopping = false;
    bool m_restartRequested = false;
    int m_retryAttempt = 0;
};
