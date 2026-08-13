#pragma once

#include <QString>
#include <QMetaType>

enum class ReceiverState {
    Stopped,
    Starting,
    Ready,
    Connecting,
    Mirroring,
    Error,
    Retrying
};

QString receiverStateLabel(ReceiverState state);
QString receiverStateColor(ReceiverState state);
bool receiverToggleStops(ReceiverState state);

class ReceiverStateMachine {
public:
    ReceiverState state() const;
    bool moveTo(ReceiverState next);
    static bool canTransition(ReceiverState from, ReceiverState to);

private:
    ReceiverState m_state = ReceiverState::Stopped;
};

Q_DECLARE_METATYPE(ReceiverState)
