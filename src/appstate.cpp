#include "appstate.h"

QString receiverStateLabel(ReceiverState state) {
    switch (state) {
    case ReceiverState::Stopped: return QStringLiteral("Stopped");
    case ReceiverState::Starting: return QStringLiteral("Starting");
    case ReceiverState::Ready: return QStringLiteral("Ready to mirror");
    case ReceiverState::Connecting: return QStringLiteral("Connecting");
    case ReceiverState::Mirroring: return QStringLiteral("Screen sharing");
    case ReceiverState::Error: return QStringLiteral("Needs attention");
    case ReceiverState::Retrying: return QStringLiteral("Reconnecting");
    }
    return QStringLiteral("Unknown");
}

QString receiverStateColor(ReceiverState state) {
    switch (state) {
    case ReceiverState::Ready: return QStringLiteral("#38d996");
    case ReceiverState::Connecting:
    case ReceiverState::Starting:
    case ReceiverState::Retrying: return QStringLiteral("#f6c85f");
    case ReceiverState::Mirroring: return QStringLiteral("#6c8cff");
    case ReceiverState::Error: return QStringLiteral("#ff6b7a");
    case ReceiverState::Stopped: return QStringLiteral("#8290a8");
    }
    return QStringLiteral("#8290a8");
}

ReceiverState ReceiverStateMachine::state() const {
    return m_state;
}

bool ReceiverStateMachine::moveTo(ReceiverState next) {
    if (next == m_state) {
        return false;
    }
    if (!canTransition(m_state, next)) {
        return false;
    }
    m_state = next;
    return true;
}

bool ReceiverStateMachine::canTransition(ReceiverState from, ReceiverState to) {
    if (to == ReceiverState::Stopped || to == ReceiverState::Error) {
        return true;
    }
    switch (from) {
    case ReceiverState::Stopped:
        return to == ReceiverState::Starting;
    case ReceiverState::Starting:
        return to == ReceiverState::Ready || to == ReceiverState::Retrying;
    case ReceiverState::Ready:
        return to == ReceiverState::Connecting || to == ReceiverState::Mirroring ||
               to == ReceiverState::Starting;
    case ReceiverState::Connecting:
        return to == ReceiverState::Mirroring || to == ReceiverState::Ready;
    case ReceiverState::Mirroring:
        return to == ReceiverState::Ready || to == ReceiverState::Connecting;
    case ReceiverState::Error:
        return to == ReceiverState::Retrying || to == ReceiverState::Starting ||
               to == ReceiverState::Ready;
    case ReceiverState::Retrying:
        return to == ReceiverState::Starting || to == ReceiverState::Ready;
    }
    return false;
}
