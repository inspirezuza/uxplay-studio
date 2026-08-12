#pragma once

#include <QtGlobal>

class SessionClock final {
public:
    void start(qint64 localMicroseconds);
    qint64 localTimestamp(qint64 localMicroseconds);
    qint64 remoteTimestamp(qint64 remoteMicroseconds, qint64 arrivalMicroseconds);

private:
    qint64 clamp(qint64 value);
    qint64 m_localStart = -1;
    qint64 m_remoteStart = -1;
    qint64 m_remoteArrivalStart = -1;
    qint64 m_last = 0;
};
