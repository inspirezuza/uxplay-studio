#include "recording/sessionclock.h"

#include <QtTest>

class SessionClockTest final : public QObject {
    Q_OBJECT
private slots:
    void mapsRemoteAndLocalTimeToOneMonotonicTimeline() {
        SessionClock clock;
        clock.start(1'000'000);
        QCOMPARE(clock.localTimestamp(1'025'000), qint64(25'000));
        QCOMPARE(clock.remoteTimestamp(9'000'000, 1'030'000), qint64(30'000));
        QCOMPARE(clock.remoteTimestamp(9'020'000, 1'050'000), qint64(50'000));
    }

    void neverMovesBackwardsAcrossRemoteDiscontinuity() {
        SessionClock clock;
        clock.start(1000);
        QCOMPARE(clock.remoteTimestamp(5000, 2000), qint64(1000));
        QCOMPARE(clock.remoteTimestamp(100, 3000), qint64(2000));
        QCOMPARE(clock.remoteTimestamp(90, 3100), qint64(2100));
    }
};

QTEST_APPLESS_MAIN(SessionClockTest)
#include "test_sessionclock.moc"
