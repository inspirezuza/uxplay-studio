#include "receiverconfig.h"

#include <QtTest>

class ReceiverConfigTest final : public QObject {
    Q_OBJECT

private slots:
    void alwaysUsesEmbeddedD3d11Renderer() {
        ReceiverConfig config;
        const QStringList args = config.uxplayArguments();
        const int sink = args.indexOf(QStringLiteral("-vs"));
        QVERIFY(sink >= 0);
        QCOMPARE(args.value(sink + 1), QStringLiteral("d3d11videosink"));
        QCOMPARE(args.value(args.indexOf(QStringLiteral("-vd")) + 1),
                 QStringLiteral("decodebin"));
        QCOMPARE(args.value(args.indexOf(QStringLiteral("-vc")) + 1),
                 QStringLiteral("d3d11convert"));
        QVERIFY(!args.contains(QStringLiteral("-fs")));
    }

    void buildsLowLatencyProfile() {
        ReceiverConfig config;
        QCOMPARE(config.quality, QualityProfile::LowLatency1080p60);
        QVERIFY(config.usesLowLatencyPipeline());
        config.quality = QualityProfile::LowLatency1080p60;
        const QStringList args = config.uxplayArguments();
        QCOMPARE(args.mid(args.indexOf(QStringLiteral("-vsync")), 2),
                 QStringList({QStringLiteral("-vsync"), QStringLiteral("no")}));
        QVERIFY(args.contains(QStringLiteral("1920x1080@60")));
    }

    void buildsUltraLowLatencyProfileForBusyWifi() {
        ReceiverConfig config;
        config.quality = QualityProfile::UltraLowLatency720p30;
        QVERIFY(config.usesLowLatencyPipeline());
        const QStringList args = config.uxplayArguments();
        QCOMPARE(args.mid(args.indexOf(QStringLiteral("-vsync")), 2),
                 QStringList({QStringLiteral("-vsync"), QStringLiteral("no")}));
        QVERIFY(args.contains(QStringLiteral("1280x720@30")));
        QCOMPARE(args.value(args.indexOf(QStringLiteral("-fps")) + 1), QStringLiteral("30"));
    }

    void balancedProfileRetainsTimestampSync() {
        ReceiverConfig config;
        config.quality = QualityProfile::Balanced1080p60;
        QVERIFY(!config.usesLowLatencyPipeline());
        QVERIFY(!config.uxplayArguments().contains(QStringLiteral("-vsync")));
    }

    void validatesSharedNetworkPin() {
        ReceiverConfig config;
        config.pinEnabled = true;
        config.pin = QStringLiteral("12");
        QVERIFY(!config.validationError().isEmpty());
        config.pin = QStringLiteral("1208");
        QVERIFY(config.validationError().isEmpty());
        QVERIFY(config.uxplayArguments().contains(QStringLiteral("-pin")));
    }
};

QTEST_GUILESS_MAIN(ReceiverConfigTest)
#include "test_receiverconfig.moc"
