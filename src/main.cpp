#include "mainwindow.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QTextStream>
#include <QTimer>
#include <QTemporaryDir>

#include <gst/gst.h>
#include <gst/video/videooverlay.h>

#include <algorithm>
#include <memory>

#ifdef Q_OS_WIN
#include <windows.h>
#include <cstdio>
#endif

static int runRuntimeSelfTest(const QString &appPath) {
    const QStringList requiredFiles {
        QStringLiteral("uxplay-bluetooth-beacon.exe"),
        QStringLiteral("dnssd.dll"),
        QStringLiteral("mDNSResponder.exe"),
        QStringLiteral("platforms/qwindows.dll"),
        QStringLiteral("libexec/gstreamer-1.0/gst-plugin-scanner.exe"),
        QStringLiteral("resources/gstreamer-features.txt"),
        QStringLiteral("resources/gstreamer-plugins.json"),
        QStringLiteral("resources/build-manifest.json"),
        QStringLiteral("resources/bundle-files.json")
    };

    bool passed = true;
    for (const QString &relativePath : requiredFiles) {
        if (!QFileInfo::exists(QDir(appPath).filePath(relativePath))) {
            fprintf(stderr, "SELF-TEST ERROR: missing %s\n",
                    relativePath.toUtf8().constData());
            passed = false;
        }
    }

#ifdef Q_OS_WIN
    const QString dnssdPath = QDir::toNativeSeparators(QDir(appPath).filePath("dnssd.dll"));
    HMODULE dnssd = LoadLibraryW(reinterpret_cast<LPCWSTR>(dnssdPath.utf16()));
    if (!dnssd) {
        fprintf(stderr, "SELF-TEST ERROR: dnssd.dll could not be loaded\n");
        passed = false;
    } else {
        FreeLibrary(dnssd);
    }
#endif

    gst_init(nullptr, nullptr);
    GstRegistry *registry = gst_registry_get();
    QFile featureFile(QDir(appPath).filePath("resources/gstreamer-features.txt"));
    if (!featureFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        fprintf(stderr, "SELF-TEST ERROR: cannot read GStreamer feature list\n");
        passed = false;
    } else {
        QTextStream stream(&featureFile);
        while (!stream.atEnd()) {
            const QString featureName = stream.readLine().trimmed();
            if (featureName.isEmpty() || featureName.startsWith('#')) continue;
            const QByteArray utf8 = featureName.toUtf8();
            GstPluginFeature *feature = gst_registry_find_feature(
                registry, utf8.constData(), GST_TYPE_ELEMENT_FACTORY);
            if (!feature) {
                fprintf(stderr, "SELF-TEST ERROR: GStreamer feature missing: %s\n", utf8.constData());
                passed = false;
            } else {
                gst_object_unref(feature);
            }
        }
    }

    GstElement *embeddedSink = gst_element_factory_make("d3d11videosink", nullptr);
    if (!embeddedSink || !GST_IS_VIDEO_OVERLAY(embeddedSink)) {
        fprintf(stderr, "SELF-TEST ERROR: d3d11videosink cannot embed in the app window\n");
        passed = false;
    }
    if (embeddedSink) {
        gst_object_unref(embeddedSink);
    }

    if (passed) {
        fprintf(stdout, "SELF-TEST OK: UxPlay Studio runtime bundle is complete\n");
        return 0;
    }
    return 2;
}

int main(int argc, char *argv[]) {
#ifdef Q_OS_WIN
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE *stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
    }
#endif

    QApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    const bool snapshotMode = std::any_of(
        arguments.cbegin(), arguments.cend(), [](const QString &argument) {
            return argument == QStringLiteral("--snapshot-edit") ||
                   argument == QStringLiteral("--snapshot-fullscreen") ||
                   argument.startsWith(QStringLiteral("--ui-snapshot="));
        });
    app.setOrganizationName(QStringLiteral("inspirezuza"));
    app.setOrganizationDomain(QStringLiteral("github.com/inspirezuza"));
    app.setApplicationName(QStringLiteral("UxPlay Studio"));
    app.setApplicationDisplayName(QStringLiteral("UxPlay Studio"));
    app.setApplicationVersion(QStringLiteral(UXPLAY_STUDIO_VERSION));
    app.setWindowIcon(QIcon(QDir(QApplication::applicationDirPath())
                                .filePath(QStringLiteral("resources/icon.ico"))));

    const QString appPath = QApplication::applicationDirPath();
    const QString pluginPath = QDir::toNativeSeparators(appPath + "/lib/gstreamer-1.0");
    qputenv("GST_PLUGIN_PATH", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_PATH_1_0", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_SYSTEM_PATH", pluginPath.toUtf8());
    qputenv("GST_PLUGIN_SYSTEM_PATH_1_0", pluginPath.toUtf8());
    const QString scannerPath = QDir::toNativeSeparators(
        appPath + "/libexec/gstreamer-1.0/gst-plugin-scanner.exe");
    qputenv("GST_PLUGIN_SCANNER", scannerPath.toUtf8());
    qputenv("GST_PLUGIN_SCANNER_1_0", scannerPath.toUtf8());
    qputenv("GIO_EXTRA_MODULES",
            QDir::toNativeSeparators(appPath + "/lib/gio/modules").toUtf8());
    qputenv("FONTCONFIG_PATH", QDir::toNativeSeparators(appPath + "/etc/fonts").toUtf8());
    const QString path = QDir::toNativeSeparators(appPath) + ";" +
                         QProcessEnvironment::systemEnvironment().value("PATH");
    qputenv("PATH", path.toUtf8());

    if (arguments.contains(QStringLiteral("--self-test"))) {
        return runRuntimeSelfTest(appPath);
    }

    QFile theme(QStringLiteral(":/theme.qss"));
    if (theme.open(QIODevice::ReadOnly | QIODevice::Text)) {
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));
    }
    app.setQuitOnLastWindowClosed(false);

#ifdef Q_OS_WIN
    HANDLE singleInstance = snapshotMode
        ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\UxPlayStudio.SingleInstance");
    if (!snapshotMode && singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existing = FindWindowW(nullptr, L"UxPlay Studio")) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        CloseHandle(singleInstance);
        return 0;
    }
#endif

    std::unique_ptr<QTemporaryDir> snapshotProjects;
    if (snapshotMode) snapshotProjects = std::make_unique<QTemporaryDir>();
    MainWindow window(nullptr,
                      !snapshotMode && !arguments.contains(QStringLiteral("--no-autostart")),
                      snapshotProjects ? snapshotProjects->path() : QString());
    window.show();
    if (arguments.contains(QStringLiteral("--snapshot-edit"))) {
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Edit layout")) button->click();
    }
    if (arguments.contains(QStringLiteral("--snapshot-fullscreen"))) {
        if (auto *button = window.findChild<QPushButton *>(QStringLiteral("fullscreenButton")))
            button->click();
    }
    for (const QString &argument : arguments) {
        if (!argument.startsWith(QStringLiteral("--ui-snapshot="))) continue;
        const QString output = argument.mid(QStringLiteral("--ui-snapshot=").size());
        QTimer::singleShot(250, &window, [&window, output]() {
            window.grab().save(output, "PNG");
            qApp->quit();
        });
    }
    const int result = app.exec();
#ifdef Q_OS_WIN
    if (singleInstance) CloseHandle(singleInstance);
#endif
    return result;
}
