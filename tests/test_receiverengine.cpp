#include "receiverengine.h"
#include "uxplay_api.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QProcess>
#include <QtTest>

#include <atomic>
#include <functional>
#include <memory>

extern "C" int start_uxplay(int, char *[]) {
    return 0;
}

extern "C" void stop_uxplay() {}

extern "C" void uxplay_set_video_window(uintptr_t) {}

extern "C" void uxplay_set_event_callback(uxplay_event_callback, void *) {}

extern "C" void uxplay_set_preview_callback(uxplay_preview_callback, void *) {}

namespace {

constexpr auto helperArgument = "--slow-worker-shutdown-helper";
constexpr int workerExitDelayMs = 500;
constexpr int shutdownGraceMs = 20;
constexpr int maximumEngineDestructionMs = 300;

struct WorkerState {
    std::atomic_bool started = false;
    std::atomic_bool stopRequested = false;
    std::atomic_bool destroyed = false;
};

class DelayedExitWorker final : public AirPlayWorker {
public:
    explicit DelayedExitWorker(std::shared_ptr<WorkerState> state)
        : m_state(std::move(state)) {}

    ~DelayedExitWorker() override {
        m_state->destroyed = true;
    }

    void stopAirplay() override {
        m_state->stopRequested = true;
        requestInterruption();
    }

protected:
    void run() override {
        m_state->started = true;
        while (!isInterruptionRequested()) {
            QThread::msleep(1);
        }

        QThread::msleep(workerExitDelayMs);

        // Exercise the queued worker-to-engine connection after the engine has
        // already been destroyed. QObject must discard it with the receiver.
        emit receiverEvent(ReceiverEvent {});
    }

private:
    std::shared_ptr<WorkerState> m_state;
};

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(1);
    }
    return condition();
}

int runSlowWorkerShutdownHelper() {
    auto state = std::make_shared<WorkerState>();
    auto *engine = new ReceiverEngine(
        [state]() -> AirPlayWorker * { return new DelayedExitWorker(state); },
        shutdownGraceMs);
    engine->start(ReceiverConfig {}, 1, {});
    if (!waitUntil([state]() { return state->started.load(); }, 3000)) {
        delete engine;
        return 2;
    }

    QElapsedTimer destructionTimer;
    destructionTimer.start();
    delete engine;
    if (destructionTimer.elapsed() >= maximumEngineDestructionMs) {
        return 3;
    }
    if (!state->stopRequested) {
        return 4;
    }
    if (!waitUntil([state]() { return state->destroyed.load(); }, 3000)) {
        return 5;
    }
    return 0;
}

} // namespace

class ReceiverEngineTest final : public QObject {
    Q_OBJECT

private slots:
    void destroyingEngineDoesNotDestroyDelayedWorker() {
        QProcess helper;
        helper.setProcessChannelMode(QProcess::MergedChannels);
        helper.start(QCoreApplication::applicationFilePath(), {QString::fromLatin1(helperArgument)});

        QVERIFY2(helper.waitForStarted(3000), qPrintable(helper.errorString()));
        QVERIFY2(helper.waitForFinished(10000), qPrintable(helper.errorString()));

        const QByteArray output = helper.readAll();
        QCOMPARE(helper.exitStatus(), QProcess::NormalExit);
        QVERIFY2(helper.exitCode() == 0, output.constData());
    }
};

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains(QString::fromLatin1(helperArgument))) {
        return runSlowWorkerShutdownHelper();
    }

    ReceiverEngineTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_receiverengine.moc"
