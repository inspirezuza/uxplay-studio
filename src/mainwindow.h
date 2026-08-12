#pragma once

#include "appstate.h"
#include "receiverconfig.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QMainWindow>
#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QCloseEvent;
class QKeyEvent;
class QMoveEvent;
class QResizeEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QHBoxLayout;
class QPlainTextEdit;
class QProcess;
class QPushButton;
class QStackedWidget;
class QSystemTrayIcon;
class QTextEdit;
class QThread;
class QVBoxLayout;
class QWidget;
class ReceiverEngine;
class VideoSurface;
class SceneCanvas;
class SceneDocument;
class ProjectStore;
class GstPipelineRunner;
class RecordingSession;
class ExportJob;
struct ProjectInfo;
struct ReceiverEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr, bool autoStart = true,
                        const QString &projectRootOverride = {});
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void moveEvent(QMoveEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupUi();
    void setupTray();
    QWidget *createPlayerPage();
    QWidget *createActivityPage();
    QWidget *createProjectsPage();
    QWidget *createSettingsPage();
    QWidget *createDiagnosticsPage();
    QPushButton *createNavigationButton(const QString &text, int page);
    void selectPage(int page);
    void setStudioMode(bool edit);
    void setSceneFormat(bool vertical);
    void addStudioSource(int type);
    void refreshLayerList();
    void refreshProjectList();
    void toggleRecording();
    void exportCurrentProject();
    void saveCurrentProject();
    void refreshRecordingCapture();
    void loadRecordedPreviews(const ProjectInfo &project);

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
    bool m_cursorOverride = false;
    bool m_closeHintShown = false;
    QByteArray m_geometryBeforeFullscreen;
    Qt::WindowStates m_windowStateBeforeFullscreen;

    QWidget *m_sidebar = nullptr;
    QWidget *m_header = nullptr;
    QStackedWidget *m_pages = nullptr;
    QList<QPushButton *> m_navigationButtons;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_statusBadge = nullptr;
    QLabel *m_sidebarReceiver = nullptr;
    QPushButton *m_receiverToggle = nullptr;

    VideoSurface *m_videoSurface = nullptr;
    QWidget *m_playerPage = nullptr;
    QWidget *m_playerCard = nullptr;
    QWidget *m_playerChrome = nullptr;
    QWidget *m_playerControls = nullptr;
    QHBoxLayout *m_playerPageLayout = nullptr;
    QVBoxLayout *m_playerLayout = nullptr;
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

    std::unique_ptr<SceneDocument> m_sceneDocument;
    std::unique_ptr<ProjectStore> m_projectStore;
    std::unique_ptr<GstPipelineRunner> m_pipelineRunner;
    RecordingSession *m_recordingSession = nullptr;
    ExportJob *m_exportJob = nullptr;
    std::unique_ptr<ProjectInfo> m_currentProject;
    SceneCanvas *m_sceneCanvas = nullptr;
    QStackedWidget *m_previewStack = nullptr;
    QListWidget *m_sourceList = nullptr;
    QListWidget *m_layerList = nullptr;
    QListWidget *m_projectList = nullptr;
    QLabel *m_recordingStatus = nullptr;
    QPushButton *m_recordButton = nullptr;
    QPushButton *m_liveModeButton = nullptr;
    QPushButton *m_editModeButton = nullptr;
    QPushButton *m_wideButton = nullptr;
    QPushButton *m_verticalButton = nullptr;
    QCheckBox *m_recordCamera = nullptr;
    QCheckBox *m_recordMicrophone = nullptr;
    int m_sceneFormat = 0;
    QList<QThread *> m_previewThreads;
};
