#include "streamhealth.h"

#include <algorithm>

void StreamHealthMonitor::setMirroring(bool mirroring, qint64) {
    if (m_mirroring == mirroring) {
        return;
    }
    m_mirroring = mirroring;
    m_locked = false;
    m_devicePaused = false;
    m_restoring = false;
    m_deviceResuming = false;
    m_restartIssued = false;
    m_lastFrameMs = -1;
    m_resumeStartedMs = -1;
}

void StreamHealthMonitor::frameReceived(qint64 nowMs) {
    if (!m_mirroring) {
        return;
    }
    m_lastFrameMs = nowMs;
    if (!m_locked && !m_devicePaused &&
        (m_restoring || m_deviceResuming) && !m_restartIssued) {
        m_restoring = false;
        m_deviceResuming = false;
        m_resumeStartedMs = -1;
    }
}

StreamHealthMonitor::Action StreamHealthMonitor::sessionLocked(qint64) {
    if (!m_mirroring) {
        return Action::None;
    }
    m_locked = true;
    m_restoring = false;
    m_restartIssued = false;
    m_resumeStartedMs = -1;
    return Action::None;
}

StreamHealthMonitor::Action StreamHealthMonitor::sessionResumed(qint64 nowMs) {
    if (!m_mirroring || m_restoring || m_restartIssued) {
        m_locked = false;
        return Action::None;
    }
    m_locked = false;
    m_restoring = true;
    m_resumeStartedMs = nowMs;
    return Action::RefreshRenderer;
}

void StreamHealthMonitor::devicePaused(qint64) {
    if (!m_mirroring) {
        return;
    }
    m_devicePaused = true;
    m_deviceResuming = false;
    m_restartIssued = false;
    m_resumeStartedMs = -1;
}

void StreamHealthMonitor::deviceResumed(qint64 nowMs) {
    if (!m_mirroring) {
        return;
    }
    m_devicePaused = false;
    m_deviceResuming = true;
    m_restartIssued = false;
    m_resumeStartedMs = nowMs;
}

StreamHealthMonitor::Action StreamHealthMonitor::tick(qint64 nowMs) {
    if (m_mirroring && !m_devicePaused &&
        (m_restoring || m_deviceResuming) && !m_restartIssued &&
        m_resumeStartedMs >= 0 &&
        nowMs - m_resumeStartedMs >= UnlockRecoveryTimeoutMs) {
        m_restoring = false;
        m_deviceResuming = false;
        m_restartIssued = true;
        return Action::RestartReceiver;
    }
    return Action::None;
}

void StreamHealthMonitor::rendererRefreshFailed() {
    if (!m_mirroring) {
        return;
    }
    m_restoring = false;
    m_restartIssued = true;
}

StreamHealthMonitor::Health StreamHealthMonitor::health(qint64 nowMs) const {
    if (!m_mirroring) {
        return Health::Idle;
    }
    if (m_locked) {
        return Health::Locked;
    }
    if (m_devicePaused) {
        return Health::DevicePaused;
    }
    if (m_restartIssued) {
        return Health::Reconnecting;
    }
    if (m_restoring) {
        return Health::Restoring;
    }
    if (m_deviceResuming) {
        return Health::DeviceResuming;
    }
    if (m_lastFrameMs < 0) {
        return Health::WaitingForFrames;
    }
    if (nowMs - m_lastFrameMs > StaleFrameThresholdMs) {
        return Health::Stalled;
    }
    return Health::Live;
}

qint64 StreamHealthMonitor::millisecondsSinceFrame(qint64 nowMs) const {
    if (m_lastFrameMs < 0) {
        return -1;
    }
    return std::max<qint64>(0, nowMs - m_lastFrameMs);
}
