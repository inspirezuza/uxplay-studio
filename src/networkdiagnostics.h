#pragma once

#include "receiverconfig.h"

#include <QString>

class NetworkDiagnostics {
public:
    static QString report(const ReceiverConfig &config, quintptr videoWindow);
    static QString primaryAddress();
    static bool bonjourServiceAvailable();
};
