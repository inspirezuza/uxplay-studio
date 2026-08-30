#pragma once

#include <QtGlobal>

class StreamHealthMonitor final {
public:
    enum class Health {
        Idle,
        WaitingForFrames,
        Live,
        Locked,
        DevicePaused,
        Restoring,
        DeviceResuming,
        Reconnecting,
        Stalled
    };

    enum class Action {
        None,
        RefreshRenderer,
        RestartReceiver
    };

    static constexpr qint64 StaleFrameThresholdMs = 5000;
    static constexpr qint64 UnlockRecoveryTimeoutMs = 7000;

    void setMirroring(bool mirroring, qint64 nowMs);
    void frameReceived(qint64 nowMs);
    Action sessionLocked(qint64 nowMs);
    Action sessionResumed(qint64 nowMs);
    void devicePaused(qint64 nowMs);
    void deviceResumed(qint64 nowMs);
    Action tick(qint64 nowMs);
    void rendererRefreshFailed();

    Health health(qint64 nowMs) const;
    qint64 millisecondsSinceFrame(qint64 nowMs) const;

private:
    bool m_mirroring = false;
    bool m_locked = false;
    bool m_devicePaused = false;
    bool m_restoring = false;
    bool m_deviceResuming = false;
    bool m_restartIssued = false;
    qint64 m_lastFrameMs = -1;
    qint64 m_resumeStartedMs = -1;
};
