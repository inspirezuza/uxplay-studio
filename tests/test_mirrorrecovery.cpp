#include "mirror_recovery.h"

#include <QtTest>

class MirrorRecoveryTest final : public QObject {
    Q_OBJECT

private slots:
    void forwardsNormallyUntilResumeGateStarts() {
        mirror_resume_gate_t gate {false};
        const uint8_t deltaFrame[] {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        QVERIFY(mirror_resume_gate_should_forward(
            &gate, deltaFrame, sizeof(deltaFrame)));
    }

    void dropsDeltaFramesUntilFirstKeyframe() {
        mirror_resume_gate_t gate {false};
        const uint8_t deltaFrame[] {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
        const uint8_t keyframe[] {0x00, 0x00, 0x00, 0x00, 0x00, 0x10};

        mirror_resume_gate_begin(&gate);
        QVERIFY(!mirror_resume_gate_should_forward(
            &gate, deltaFrame, sizeof(deltaFrame)));
        QVERIFY(gate.awaiting_keyframe);
        QVERIFY(mirror_resume_gate_should_forward(
            &gate, keyframe, sizeof(keyframe)));
        QVERIFY(!gate.awaiting_keyframe);
        QVERIFY(mirror_resume_gate_should_forward(
            &gate, deltaFrame, sizeof(deltaFrame)));
    }

    void rejectsTruncatedHeadersWhileWaiting() {
        mirror_resume_gate_t gate {true};
        const uint8_t truncated[] {0x00, 0x00, 0x00, 0x00, 0x00};
        QVERIFY(!mirror_resume_gate_should_forward(
            &gate, truncated, sizeof(truncated)));
        QVERIFY(gate.awaiting_keyframe);
    }
};

QTEST_APPLESS_MAIN(MirrorRecoveryTest)
#include "test_mirrorrecovery.moc"
