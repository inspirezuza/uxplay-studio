#include "appstate.h"

#include <QtTest>

class AppStateTest final : public QObject {
    Q_OBJECT

private slots:
    void followsNormalMirroringLifecycle() {
        ReceiverStateMachine state;
        QCOMPARE(state.state(), ReceiverState::Stopped);
        QVERIFY(state.moveTo(ReceiverState::Starting));
        QVERIFY(state.moveTo(ReceiverState::Ready));
        QVERIFY(state.moveTo(ReceiverState::Connecting));
        QVERIFY(state.moveTo(ReceiverState::Mirroring));
        QVERIFY(state.moveTo(ReceiverState::Ready));
        QVERIFY(state.moveTo(ReceiverState::Stopped));
    }

    void rejectsImpossibleJump() {
        ReceiverStateMachine state;
        QVERIFY(!state.moveTo(ReceiverState::Mirroring));
        QCOMPARE(state.state(), ReceiverState::Stopped);
    }

    void supportsCappedRecoveryLifecycle() {
        ReceiverStateMachine state;
        QVERIFY(state.moveTo(ReceiverState::Starting));
        QVERIFY(state.moveTo(ReceiverState::Error));
        QVERIFY(state.moveTo(ReceiverState::Retrying));
        QVERIFY(state.moveTo(ReceiverState::Starting));
    }
};

QTEST_GUILESS_MAIN(AppStateTest)
#include "test_appstate.moc"
