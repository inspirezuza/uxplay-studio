#pragma once

#include <QString>
#include <QStringList>

enum class QualityProfile {
    Balanced1080p60 = 0,
    Efficient720p30 = 1,
    LowLatency1080p60 = 2,
    UltraLowLatency720p30 = 3
};

struct ReceiverConfig {
    QString receiverName = QStringLiteral("UxPlay Studio");
    QualityProfile quality = QualityProfile::LowLatency1080p60;
    bool pinEnabled = false;
    QString pin = QStringLiteral("2468");
    bool bluetoothDiscovery = true;
    bool autostart = false;
    bool notifications = true;

    QString validationError() const;
    QStringList uxplayArguments(const QString &bleStatusPath = {}) const;
    QString qualityLabel() const;
    bool usesLowLatencyPipeline() const;
};

class SettingsStore {
public:
    static ReceiverConfig load();
    static void save(const ReceiverConfig &config);
};
