#pragma once

#include "appstate.h"
#include "receiverconfig.h"

#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>

class QAction;
class QCheckBox;
class QComboBox;
class QCloseEvent;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class QTextEdit;
class QWidget;
class ReceiverEngine;
class VideoSurface;
struct ReceiverEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, bool autoStart = true);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void setupUi();
    void setupTray();
    QWidget *createPlayerPage();
    QWidget *createActivityPage();
    QWidget *createSettingsPage();
    QWidget *createDiagnosticsPage();
    QPushButton *createNavigationButton(const QString &text, int page);
    void selectPage(int page);

    void startReceiver();
    void stopReceiver();
    void restartReceiver();
    void toggleReceiver();
    void handleStateChanged(ReceiverState state);
    void handleReceiverEvent(const ReceiverEvent &event);
    void updateSessionTimer();
    void appendActivity(const QString &category, const QString &message);

    ReceiverConfig configFromControls() const;
    void loadConfigIntoControls();
    void saveSettings();
    void updateSecuritySummary();
    void refreshDiagnostics();

    QString bleStatusPath() const;
    void startBluetoothBeacon();
    void stopBluetoothBeacon();
    bool ensureBonjourAvailable();
    bool autostartEnabled() const;
    void setAutostart(bool enabled);

    void enterFullscreen();
    void exitFullscreen();
    void showFromTray();
    void quitApplication();

    ReceiverEngine *m_engine = nullptr;
    ReceiverConfig m_config;
    QProcess *m_beacon = nullptr;
    bool m_autoStart = true;
    bool m_quitting = false;
    bool m_fullscreen = false;
    bool m_closeHintShown = false;

    QWidget *m_sidebar = nullptr;
    QWidget *m_header = nullptr;
    QStackedWidget *m_pages = nullptr;
    QList<QPushButton *> m_navigationButtons;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_sidebarReceiver = nullptr;
    QPushButton *m_receiverToggle = nullptr;

    VideoSurface *m_videoSurface = nullptr;
    QWidget *m_sessionPanel = nullptr;
    QLabel *m_sessionState = nullptr;
    QLabel *m_deviceName = nullptr;
    QLabel *m_deviceModel = nullptr;
    QLabel *m_resolution = nullptr;
    QLabel *m_duration = nullptr;
    QLabel *m_networkAddress = nullptr;
    QLabel *m_securitySummary = nullptr;
    QPushButton *m_fullscreenButton = nullptr;

    QTextEdit *m_activityLog = nullptr;
    QPlainTextEdit *m_diagnostics = nullptr;

    QLineEdit *m_receiverNameEdit = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QCheckBox *m_pinEnabledCheck = nullptr;
    QLineEdit *m_pinEdit = nullptr;
    QCheckBox *m_bluetoothCheck = nullptr;
    QCheckBox *m_autostartCheck = nullptr;
    QCheckBox *m_notificationsCheck = nullptr;
    QLabel *m_settingsFeedback = nullptr;

    QSystemTrayIcon *m_tray = nullptr;
    QAction *m_trayReceiverAction = nullptr;
    QElapsedTimer m_sessionElapsed;
};
