#include "sessionclock.h"

#include <algorithm>

void SessionClock::start(qint64 localMicroseconds) {
    m_localStart = localMicroseconds;
    m_remoteStart = -1;
    m_remoteArrivalStart = -1;
    m_last = 0;
}

qint64 SessionClock::clamp(qint64 value) {
    m_last = std::max(m_last, value);
    return m_last;
}

qint64 SessionClock::localTimestamp(qint64 localMicroseconds) {
    if (m_localStart < 0) start(localMicroseconds);
    return clamp(std::max<qint64>(0, localMicroseconds - m_localStart));
}

qint64 SessionClock::remoteTimestamp(qint64 remoteMicroseconds, qint64 arrivalMicroseconds) {
    if (m_localStart < 0) start(arrivalMicroseconds);
    if (m_remoteStart < 0 || remoteMicroseconds < m_remoteStart) {
        m_remoteStart = remoteMicroseconds;
        m_remoteArrivalStart = arrivalMicroseconds;
    }
    const qint64 remoteDelta = remoteMicroseconds - m_remoteStart;
    const qint64 localBase = m_remoteArrivalStart - m_localStart;
    return clamp(std::max<qint64>(0, localBase + remoteDelta));
}
