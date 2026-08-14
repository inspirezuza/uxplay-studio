#include "networkdiagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QNetworkAddressEntry>
#include <QNetworkInterface>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QSysInfo>
#include <QTextStream>
#include <QtGlobal>
#include <limits>
#include <gst/gst.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winsvc.h>
#endif

namespace {
bool isPrivateIpv4(const QString &address) {
    const QStringList octets = address.split(QLatin1Char('.'));
    if (octets.size() != 4) return false;
    bool ok = false;
    const int first = octets.at(0).toInt(&ok);
    if (!ok) return false;
    if (first == 10 || first == 192) return true;
    if (first != 172) return false;
    const int second = octets.at(1).toInt(&ok);
    return ok && second >= 16 && second <= 31;
}

int addressPreference(const QNetworkInterface &networkInterface, const QString &address) {
    const QString name = networkInterface.humanReadableName().toLower();
    int score = networkInterface.flags().testFlag(QNetworkInterface::IsRunning) ? 10 : 0;
    if (name.contains(QStringLiteral("wi-fi")) || name.contains(QStringLiteral("wifi")) ||
        name.contains(QStringLiteral("wlan"))) {
        score += 100;
    } else if (name.contains(QStringLiteral("ethernet"))) {
        score += 50;
    }
    if (isPrivateIpv4(address)) score += 30;
    if (address.startsWith(QStringLiteral("169.254."))) score -= 200;
    if (name.contains(QStringLiteral("virtual")) || name.contains(QStringLiteral("vethernet")) ||
        name.contains(QStringLiteral("bluetooth")) || name.contains(QStringLiteral("vpn")) ||
        name.contains(QStringLiteral("loopback"))) {
        score -= 120;
    }
    return score;
}
}

QString NetworkDiagnostics::primaryAddress() {
    QString preferred;
    int preferredScore = std::numeric_limits<int>::min();
    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        const auto flags = networkInterface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            flags.testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.ip().isLoopback()) {
                continue;
            }
            const QString address = entry.ip().toString();
            const int score = addressPreference(networkInterface, address);
            if (score > preferredScore) {
                preferred = address;
                preferredScore = score;
            }
        }
    }
    return preferred.isEmpty() ? QStringLiteral("No active IPv4 address") : preferred;
}

bool NetworkDiagnostics::bonjourServiceAvailable() {
#ifdef Q_OS_WIN
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) {
        return false;
    }
    SC_HANDLE service = OpenServiceW(manager, L"Bonjour Service", SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }
    SERVICE_STATUS status {};
    const bool running = QueryServiceStatus(service, &status) &&
                         status.dwCurrentState == SERVICE_RUNNING;
    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return running;
#else
    return true;
#endif
}

QString NetworkDiagnostics::report(const ReceiverConfig &config, quintptr videoWindow) {
    QString output;
    QTextStream stream(&output);
    stream << "UxPlay Studio diagnostics\n";
    stream << "Generated: " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n\n";
    stream << "Application\n";
    stream << "  Version: " << QCoreApplication::applicationVersion() << "\n";
    stream << "  Qt: " << qVersion() << "\n";
    stream << "  GStreamer: " << gst_version_string() << "\n";
    stream << "  OS: " << QSysInfo::prettyProductName() << "\n";
    stream << "  Architecture: " << QSysInfo::currentCpuArchitecture() << "\n\n";
    stream << "Receiver\n";
    stream << "  Name: " << config.receiverName << "\n";
    stream << "  Quality: " << config.qualityLabel() << "\n";
    stream << "  Renderer: d3d11videosink (embedded)\n";
    stream << "  Decoder: Automatic; hardware preferred with software fallback\n";
    stream << "  Frame backlog: "
           << (config.usesLowLatencyPipeline() ? "Bounded to 2 decoded frames; stale frames drop"
                                               : "Timestamp-synchronized")
           << "\n";
    stream << "  Native video handle: 0x" << QString::number(videoWindow, 16) << "\n";
    stream << "  Access control: " << (config.pinEnabled ? "4-digit PIN" : "Open") << "\n";
    stream << "  Bluetooth discovery: " << (config.bluetoothDiscovery ? "Enabled" : "Disabled") << "\n\n";
    stream << "Discovery\n";
    stream << "  Bonjour Service: " << (bonjourServiceAvailable() ? "Running" : "Unavailable") << "\n";
    stream << "  Primary IPv4: " << primaryAddress() << "\n";

    for (const QNetworkInterface &networkInterface : QNetworkInterface::allInterfaces()) {
        if (!networkInterface.flags().testFlag(QNetworkInterface::IsUp) ||
            networkInterface.flags().testFlag(QNetworkInterface::IsLoopBack)) {
            continue;
        }
        stream << "  Interface: " << networkInterface.humanReadableName() << "\n";
        for (const QNetworkAddressEntry &entry : networkInterface.addressEntries()) {
            if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                stream << "    " << entry.ip().toString() << " / "
                       << entry.netmask().toString() << "\n";
            }
        }
    }

#ifdef Q_OS_WIN
    QProcess wlan;
    wlan.start(QStringLiteral("netsh"), {QStringLiteral("wlan"), QStringLiteral("show"),
                                         QStringLiteral("interfaces")});
    if (wlan.waitForFinished(1500)) {
        const QString text = QString::fromLocal8Bit(wlan.readAllStandardOutput());
        stream << "\nWi-Fi (netsh)\n";
        const QStringList lines = text.split('\n');
        for (const QString &line : lines) {
            const QString trimmed = line.trimmed();
            if (trimmed.startsWith(QStringLiteral("State"), Qt::CaseInsensitive) ||
                trimmed.startsWith(QStringLiteral("SSID"), Qt::CaseInsensitive) ||
                trimmed.startsWith(QStringLiteral("Signal"), Qt::CaseInsensitive) ||
                trimmed.startsWith(QStringLiteral("Channel"), Qt::CaseInsensitive)) {
                stream << "  " << trimmed << "\n";
            }
        }
    }
#endif

    stream << "\nEngine arguments\n  " << config.uxplayArguments().join(' ') << "\n";
    return output;
}
