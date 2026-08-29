#include "streamhealth.h"

#include <QtTest>

class StreamHealthTest final : public QObject {
    Q_OBJECT

private slots:
    void reportsFramesAndOrdinaryStallsWithoutRestarting() {
        StreamHealthMonitor monitor;
        monitor.setMirroring(true, 1000);
        QCOMPARE(monitor.health(1000), StreamHealthMonitor::Health::WaitingForFrames);

        monitor.frameReceived(1200);
        QCOMPARE(monitor.health(1200), StreamHealthMonitor::Health::Live);
        QCOMPARE(monitor.tick(1200 + StreamHealthMonitor::StaleFrameThresholdMs + 1),
                 StreamHealthMonitor::Action::None);
        QCOMPARE(monitor.health(1200 + StreamHealthMonitor::StaleFrameThresholdMs + 1),
                 StreamHealthMonitor::Health::Stalled);
    }

    void restoresAfterUnlockWhenFramesReturn() {
        StreamHealthMonitor monitor;
        monitor.setMirroring(true, 0);
        monitor.frameReceived(100);

        QCOMPARE(monitor.sessionLocked(200), StreamHealthMonitor::Action::None);
        QCOMPARE(monitor.health(200), StreamHealthMonitor::Health::Locked);
        QCOMPARE(monitor.sessionResumed(300),
                 StreamHealthMonitor::Action::RefreshRenderer);
        QCOMPARE(monitor.health(300), StreamHealthMonitor::Health::Restoring);

        monitor.frameReceived(400);
        QCOMPARE(monitor.health(400), StreamHealthMonitor::Health::Live);
        QCOMPARE(monitor.tick(300 + StreamHealthMonitor::UnlockRecoveryTimeoutMs),
                 StreamHealthMonitor::Action::None);
    }

    void restartsOnlyOnceWhenUnlockRecoveryTimesOut() {
        StreamHealthMonitor monitor;
        monitor.setMirroring(true, 0);
        monitor.frameReceived(100);
        monitor.sessionLocked(200);
        QCOMPARE(monitor.sessionResumed(300),
                 StreamHealthMonitor::Action::RefreshRenderer);

        const qint64 deadline = 300 + StreamHealthMonitor::UnlockRecoveryTimeoutMs;
        QCOMPARE(monitor.tick(deadline - 1), StreamHealthMonitor::Action::None);
        QCOMPARE(monitor.tick(deadline), StreamHealthMonitor::Action::RestartReceiver);
        QCOMPARE(monitor.health(deadline), StreamHealthMonitor::Health::Reconnecting);
        QCOMPARE(monitor.tick(deadline + 60000), StreamHealthMonitor::Action::None);
    }

    void ignoresResumeWhenNoMirrorSessionExists() {
        StreamHealthMonitor monitor;
        QCOMPARE(monitor.sessionResumed(100), StreamHealthMonitor::Action::None);
        QCOMPARE(monitor.health(100), StreamHealthMonitor::Health::Idle);
    }

    void failedRendererRefreshMovesDirectlyToSingleReconnect() {
        StreamHealthMonitor monitor;
        monitor.setMirroring(true, 0);
        monitor.frameReceived(100);
        QCOMPARE(monitor.sessionResumed(200),
                 StreamHealthMonitor::Action::RefreshRenderer);
        QCOMPARE(monitor.sessionResumed(201), StreamHealthMonitor::Action::None);

        monitor.rendererRefreshFailed();
        QCOMPARE(monitor.health(202), StreamHealthMonitor::Health::Reconnecting);
        QCOMPARE(monitor.tick(202 + StreamHealthMonitor::UnlockRecoveryTimeoutMs),
                 StreamHealthMonitor::Action::None);
    }
};

QTEST_APPLESS_MAIN(StreamHealthTest)
#include "test_streamhealth.moc"
