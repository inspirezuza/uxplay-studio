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
        QVERIFY(!args.contains(QStringLiteral("-fs")));
    }

    void buildsLowLatencyProfile() {
        ReceiverConfig config;
        config.quality = QualityProfile::LowLatency1080p60;
        const QStringList args = config.uxplayArguments();
        QCOMPARE(args.mid(args.indexOf(QStringLiteral("-vsync")), 2),
                 QStringList({QStringLiteral("-vsync"), QStringLiteral("no")}));
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
