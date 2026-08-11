#include "receiverconfig.h"

#include <QRegularExpression>
#include <QSettings>

QString ReceiverConfig::validationError() const {
    const QString name = receiverName.trimmed();
    if (name.isEmpty()) {
        return QStringLiteral("Receiver name cannot be empty.");
    }
    if (name.size() > 48) {
        return QStringLiteral("Receiver name must be 48 characters or fewer.");
    }
    if (pinEnabled && !QRegularExpression(QStringLiteral("^[0-9]{4}$")).match(pin).hasMatch()) {
        return QStringLiteral("AirPlay PIN must contain exactly four digits.");
    }
    return {};
}

QStringList ReceiverConfig::uxplayArguments(const QString &bleStatusPath) const {
    QStringList args {
        QStringLiteral("-n"), receiverName.trimmed(),
        QStringLiteral("-nh"),
        QStringLiteral("-vd"), QStringLiteral("d3d11h264dec"),
        QStringLiteral("-vc"), QStringLiteral("d3d11convert"),
        QStringLiteral("-vs"), QStringLiteral("d3d11videosink"),
        QStringLiteral("-nofreeze")
    };

    switch (quality) {
    case QualityProfile::Efficient720p30:
        args << QStringLiteral("-s") << QStringLiteral("1280x720@30")
             << QStringLiteral("-fps") << QStringLiteral("30");
        break;
    case QualityProfile::LowLatency1080p60:
        args << QStringLiteral("-s") << QStringLiteral("1920x1080@60")
             << QStringLiteral("-fps") << QStringLiteral("60")
             << QStringLiteral("-vsync") << QStringLiteral("no");
        break;
    case QualityProfile::UltraLowLatency720p30:
        args << QStringLiteral("-s") << QStringLiteral("1280x720@30")
             << QStringLiteral("-fps") << QStringLiteral("30")
             << QStringLiteral("-vsync") << QStringLiteral("no");
        break;
    case QualityProfile::Balanced1080p60:
        args << QStringLiteral("-s") << QStringLiteral("1920x1080@60")
             << QStringLiteral("-fps") << QStringLiteral("60");
        break;
    }

    if (pinEnabled) {
        args << QStringLiteral("-pin") << pin;
    }
    if (bluetoothDiscovery && !bleStatusPath.isEmpty()) {
        args << QStringLiteral("-ble") << bleStatusPath;
    }
    return args;
}

QString ReceiverConfig::qualityLabel() const {
    switch (quality) {
    case QualityProfile::Balanced1080p60: return QStringLiteral("Balanced · 1080p 60 FPS");
    case QualityProfile::Efficient720p30: return QStringLiteral("Efficient · 720p 30 FPS");
    case QualityProfile::LowLatency1080p60: return QStringLiteral("Low latency · 1080p 60 FPS");
    case QualityProfile::UltraLowLatency720p30: return QStringLiteral("Ultra low latency · 720p 30 FPS");
    }
    return QStringLiteral("Low latency · 1080p 60 FPS");
}

bool ReceiverConfig::usesLowLatencyPipeline() const {
    return quality == QualityProfile::LowLatency1080p60
        || quality == QualityProfile::UltraLowLatency720p30;
}

ReceiverConfig SettingsStore::load() {
    QSettings settings;
    ReceiverConfig config;
    config.receiverName = settings.value(QStringLiteral("receiver/name"), config.receiverName).toString();
    const int storedQuality = settings.value(
        QStringLiteral("receiver/quality"), static_cast<int>(config.quality)).toInt();
    if (storedQuality >= static_cast<int>(QualityProfile::Balanced1080p60)
        && storedQuality <= static_cast<int>(QualityProfile::UltraLowLatency720p30)) {
        config.quality = static_cast<QualityProfile>(storedQuality);
    }
    config.pinEnabled = settings.value(QStringLiteral("security/pinEnabled"), false).toBool();
    config.pin = settings.value(QStringLiteral("security/pin"), config.pin).toString();
    config.bluetoothDiscovery = settings.value(QStringLiteral("receiver/bluetoothDiscovery"), true).toBool();
    config.autostart = settings.value(QStringLiteral("app/autostart"), false).toBool();
    config.notifications = settings.value(QStringLiteral("app/notifications"), true).toBool();
    return config;
}

void SettingsStore::save(const ReceiverConfig &config) {
    QSettings settings;
    settings.setValue(QStringLiteral("receiver/name"), config.receiverName.trimmed());
    settings.setValue(QStringLiteral("receiver/quality"), static_cast<int>(config.quality));
    settings.setValue(QStringLiteral("security/pinEnabled"), config.pinEnabled);
    settings.setValue(QStringLiteral("security/pin"), config.pin);
    settings.setValue(QStringLiteral("receiver/bluetoothDiscovery"), config.bluetoothDiscovery);
    settings.setValue(QStringLiteral("app/autostart"), config.autostart);
    settings.setValue(QStringLiteral("app/notifications"), config.notifications);
    settings.sync();
}
