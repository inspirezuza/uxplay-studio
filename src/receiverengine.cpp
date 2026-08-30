#include "receiverengine.h"

#include "uxplay_api.h"

#include <QTimer>
#include <algorithm>

ReceiverEngine::ReceiverEngine(QObject *parent) : QObject(parent) {
    qRegisterMetaType<ReceiverState>();
}

ReceiverEngine::~ReceiverEngine() {
    m_userStopping = true;
    if (m_worker && m_worker->isRunning()) {
        m_worker->stopAirplay();
        m_worker->wait(3000);
    }
}

ReceiverState ReceiverEngine::state() const {
    return m_stateMachine.state();
}

bool ReceiverEngine::isRunning() const {
    return m_worker && m_worker->isRunning();
}

void ReceiverEngine::start(const ReceiverConfig &config, quintptr videoWindow,
                           const QString &bleStatusPath) {
    if (isRunning()) {
        return;
    }
    m_config = config;
    m_videoWindow = videoWindow;
    m_bleStatusPath = bleStatusPath;
    m_userStopping = false;
    m_restartRequested = false;
    launchWorker();
}

void ReceiverEngine::stop() {
    m_userStopping = true;
    m_restartRequested = false;
    if (!m_worker || !m_worker->isRunning()) {
        transitionTo(ReceiverState::Stopped);
        return;
    }
    m_worker->stopAirplay();
}

void ReceiverEngine::restart(const ReceiverConfig &config, quintptr videoWindow,
                             const QString &bleStatusPath) {
    m_config = config;
    m_videoWindow = videoWindow;
    m_bleStatusPath = bleStatusPath;
    m_retryAttempt = 0;
    if (!m_worker || !m_worker->isRunning()) {
        m_userStopping = false;
        m_restartRequested = false;
        launchWorker();
        return;
    }
    m_userStopping = true;
    m_restartRequested = true;
    m_worker->stopAirplay();
}

bool ReceiverEngine::recoverVideoOutput() {
    if (state() != ReceiverState::Mirroring || !isRunning() || !m_videoWindow) {
        return false;
    }
    return uxplay_recover_video_window(static_cast<uintptr_t>(m_videoWindow)) != 0;
}

void ReceiverEngine::launchWorker() {
    if (isRunning() || !m_videoWindow) {
        return;
    }
    transitionTo(ReceiverState::Starting);
    auto *worker = new AirPlayWorker(this);
    m_worker = worker;
    worker->configure(m_config.uxplayArguments(m_bleStatusPath), m_videoWindow);
    connect(worker, &AirPlayWorker::receiverEvent, this, &ReceiverEngine::handleEvent,
            Qt::QueuedConnection);
    connect(worker, &AirPlayWorker::videoFrameDecoded,
            this, &ReceiverEngine::videoFrameDecoded, Qt::QueuedConnection);
    connect(worker, &AirPlayWorker::engineExited, this, &ReceiverEngine::handleWorkerExit,
            Qt::QueuedConnection);
    connect(worker, &QThread::finished, this, &ReceiverEngine::handleWorkerFinished,
            Qt::QueuedConnection);
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void ReceiverEngine::handleEvent(const ReceiverEvent &event) {
    switch (static_cast<uxplay_event_type>(event.type)) {
    case UXPLAY_EVENT_ENGINE_READY:
        m_retryAttempt = 0;
        transitionTo(ReceiverState::Ready);
        break;
    case UXPLAY_EVENT_CLIENT_CONNECTING:
    case UXPLAY_EVENT_PIN_REQUIRED:
        transitionTo(ReceiverState::Connecting);
        break;
    case UXPLAY_EVENT_MIRRORING_STARTED:
        transitionTo(ReceiverState::Mirroring);
        break;
    case UXPLAY_EVENT_STREAM_STOPPED:
        transitionTo(ReceiverState::Ready);
        break;
    case UXPLAY_EVENT_ERROR:
        transitionTo(ReceiverState::Error);
        break;
    case UXPLAY_EVENT_VIDEO_PAUSED:
    case UXPLAY_EVENT_VIDEO_RESUMING:
    case UXPLAY_EVENT_WARNING:
        break;
    }
    emit eventReceived(event);
}

void ReceiverEngine::handleWorkerExit(int exitCode) {
    if (m_userStopping) {
        return;
    }
    ReceiverEvent event;
    event.type = static_cast<int>(UXPLAY_EVENT_ERROR);
    event.message = QStringLiteral("Receiver engine exited unexpectedly (code %1).").arg(exitCode);
    emit eventReceived(event);
    transitionTo(ReceiverState::Error);
}

void ReceiverEngine::handleWorkerFinished() {
    m_worker = nullptr;
    if (m_restartRequested) {
        m_restartRequested = false;
        m_userStopping = false;
        QTimer::singleShot(200, this, &ReceiverEngine::launchWorker);
        return;
    }
    if (m_userStopping) {
        transitionTo(ReceiverState::Stopped);
        return;
    }

    const int delay = std::min(8000, 1000 * (1 << std::min(m_retryAttempt, 3)));
    ++m_retryAttempt;
    transitionTo(ReceiverState::Retrying);
    emit recoveryScheduled(delay);
    QTimer::singleShot(delay, this, [this]() {
        if (!m_userStopping && !isRunning()) {
            launchWorker();
        }
    });
}

void ReceiverEngine::transitionTo(ReceiverState next) {
    if (m_stateMachine.moveTo(next)) {
        emit stateChanged(next);
    }
}
