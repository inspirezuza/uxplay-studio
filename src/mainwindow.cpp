#include "mainwindow.h"

#include "airplayworker.h"
#include "mdns_responder.hpp"
#include "networkdiagnostics.h"
#include "receiverengine.h"
#include "streamhealth.h"
#include "videosurface.h"
#include "uxplay_api.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace {
QLabel *mutedLabel(const QString &text, QWidget *parent = nullptr) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("mutedLabel"));
    label->setWordWrap(true);
    return label;
}

QWidget *card(QWidget *parent = nullptr) {
    auto *widget = new QWidget(parent);
    widget->setObjectName(QStringLiteral("card"));
    return widget;
}

void refreshStyle(QWidget *widget) {
    if (!widget) return;
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

QString formatDuration(qint64 seconds) {
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 remaining = seconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(remaining, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remaining, 2, 10, QLatin1Char('0'));
}
}

MainWindow::MainWindow(QWidget *parent, bool autoStart)
    : QMainWindow(parent), m_autoStart(autoStart) {
    m_config = SettingsStore::load();
    m_config.autostart = autostartEnabled();
    m_engine = new ReceiverEngine(this);
    m_healthClock.start();

    setupUi();
    setupTray();
    loadConfigIntoControls();

    connect(m_engine, &ReceiverEngine::stateChanged,
            this, &MainWindow::handleStateChanged);
    connect(m_engine, &ReceiverEngine::eventReceived,
            this, &MainWindow::handleReceiverEvent);
    connect(m_engine, &ReceiverEngine::videoFrameDecoded,
            this, &MainWindow::handleVideoFrameDecoded);
    connect(m_engine, &ReceiverEngine::recoveryScheduled, this, [this](int delayMs) {
        appendActivity(QStringLiteral("Recovery"),
                       QStringLiteral("Receiver will retry in %1 second(s).")
                           .arg(delayMs / 1000));
    });

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateSessionTimer);
    timer->start(1000);

#ifdef Q_OS_WIN
    m_wtsNotificationsRegistered =
        WTSRegisterSessionNotification(reinterpret_cast<HWND>(winId()),
                                       NOTIFY_FOR_THIS_SESSION) != FALSE;
    if (!m_wtsNotificationsRegistered) {
        appendActivity(
            QStringLiteral("Recovery"),
            QStringLiteral("Windows lock notifications are unavailable; use Reconnect now if video goes black."));
    }
#endif

    handleStateChanged(ReceiverState::Stopped);
    refreshDiagnostics();
    if (m_autoStart) {
        QTimer::singleShot(0, this, &MainWindow::startReceiver);
    }
}

MainWindow::~MainWindow() {
    m_quitting = true;
#ifdef Q_OS_WIN
    if (m_wtsNotificationsRegistered) {
        WTSUnRegisterSessionNotification(reinterpret_cast<HWND>(winId()));
        m_wtsNotificationsRegistered = false;
    }
#endif
    if (m_cursorOverride) {
        QApplication::restoreOverrideCursor();
        m_cursorOverride = false;
    }
    stopBluetoothBeacon();
}

void MainWindow::setupUi() {
    setWindowTitle(QStringLiteral("UxPlay Studio"));
    setMinimumSize(960, 640);
    resize(1240, 780);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    setCentralWidget(central);
    auto *shell = new QHBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    m_sidebar = new QWidget(central);
    m_sidebar->setObjectName(QStringLiteral("sidebar"));
    m_sidebar->setFixedWidth(224);
    auto *sidebarLayout = new QVBoxLayout(m_sidebar);
    sidebarLayout->setContentsMargins(22, 26, 22, 22);
    sidebarLayout->setSpacing(8);

    auto *brand = new QLabel(QStringLiteral("UXPLAY\nSTUDIO"), m_sidebar);
    brand->setObjectName(QStringLiteral("brand"));
    sidebarLayout->addWidget(brand);
    m_sidebarReceiver = mutedLabel(m_config.receiverName, m_sidebar);
    sidebarLayout->addWidget(m_sidebarReceiver);
    sidebarLayout->addSpacing(24);

    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Player"), 0));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Activity"), 1));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Settings"), 2));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Diagnostics"), 3));
    sidebarLayout->addStretch();

    auto *opensource = mutedLabel(
        QStringLiteral("Open source · GPL-3.0\nPowered by UxPlay + GStreamer"), m_sidebar);
    opensource->setObjectName(QStringLiteral("sidebarFooter"));
    sidebarLayout->addWidget(opensource);
    shell->addWidget(m_sidebar);

    auto *content = new QWidget(central);
    content->setObjectName(QStringLiteral("content"));
    auto *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_header = new QWidget(content);
    m_header->setObjectName(QStringLiteral("header"));
    m_header->setFixedHeight(84);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(30, 18, 30, 18);
    m_pageTitle = new QLabel(QStringLiteral("Player"), m_header);
    m_pageTitle->setObjectName(QStringLiteral("pageTitle"));
    headerLayout->addWidget(m_pageTitle);
    headerLayout->addStretch();
    m_statusBadge = new QLabel(QStringLiteral("Stopped"), m_header);
    m_statusBadge->setObjectName(QStringLiteral("statusBadge"));
    headerLayout->addWidget(m_statusBadge);
    m_receiverToggle = new QPushButton(QStringLiteral("Start receiver"), m_header);
    m_receiverToggle->setObjectName(QStringLiteral("primaryButton"));
    connect(m_receiverToggle, &QPushButton::clicked, this, &MainWindow::toggleReceiver);
    headerLayout->addWidget(m_receiverToggle);
    contentLayout->addWidget(m_header);

    m_pages = new QStackedWidget(content);
    m_pages->addWidget(createPlayerPage());
    m_pages->addWidget(createActivityPage());
    m_pages->addWidget(createSettingsPage());
    m_pages->addWidget(createDiagnosticsPage());
    contentLayout->addWidget(m_pages, 1);
    shell->addWidget(content, 1);
    selectPage(0);
}

QPushButton *MainWindow::createNavigationButton(const QString &text, int page) {
    auto *button = new QPushButton(text, m_sidebar);
    button->setObjectName(QStringLiteral("navButton"));
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    connect(button, &QPushButton::clicked, this, [this, page]() { selectPage(page); });
    m_navigationButtons.append(button);
    return button;
}

void MainWindow::selectPage(int page) {
    if (!m_pages || page < 0 || page >= m_pages->count()) {
        return;
    }
    static const QStringList titles {
        QStringLiteral("Player"), QStringLiteral("Activity"),
        QStringLiteral("Settings"), QStringLiteral("Diagnostics")
    };
    m_pages->setCurrentIndex(page);
    m_pageTitle->setText(titles.value(page));
    for (int index = 0; index < m_navigationButtons.size(); ++index) {
        m_navigationButtons[index]->setChecked(index == page);
    }
    if (page == 3) {
        refreshDiagnostics();
    }
}

QWidget *MainWindow::createPlayerPage() {
    m_playerPage = new QWidget(this);
    m_playerPage->setObjectName(QStringLiteral("page"));
    m_playerPageLayout = new QHBoxLayout(m_playerPage);
    m_playerPageLayout->setObjectName(QStringLiteral("playerPageLayout"));
    m_playerPageLayout->setContentsMargins(30, 24, 30, 30);
    m_playerPageLayout->setSpacing(20);

    m_playerCard = card(m_playerPage);
    m_playerCard->setObjectName(QStringLiteral("playerCard"));
    m_playerLayout = new QVBoxLayout(m_playerCard);
    m_playerLayout->setObjectName(QStringLiteral("playerLayout"));
    m_playerLayout->setContentsMargins(18, 18, 18, 16);
    m_playerLayout->setSpacing(14);

    m_playerChrome = new QWidget(m_playerCard);
    m_playerChrome->setObjectName(QStringLiteral("playerChrome"));
    auto *titleRow = new QHBoxLayout(m_playerChrome);
    titleRow->setContentsMargins(0, 0, 0, 0);
    auto *title = new QLabel(QStringLiteral("Live screen"), m_playerChrome);
    title->setObjectName(QStringLiteral("cardTitle"));
    titleRow->addWidget(title);
    titleRow->addStretch();
    auto *embedded = new QLabel(QStringLiteral("D3D11 · EMBEDDED"), m_playerChrome);
    embedded->setObjectName(QStringLiteral("miniBadge"));
    titleRow->addWidget(embedded);
    m_playerLayout->addWidget(m_playerChrome);

    m_videoSurface = new VideoSurface(m_playerCard);
    m_playerLayout->addWidget(m_videoSurface, 1);

    m_playerControls = new QWidget(m_playerCard);
    m_playerControls->setObjectName(QStringLiteral("playerControls"));
    auto *controls = new QHBoxLayout(m_playerControls);
    controls->setContentsMargins(0, 0, 0, 0);
    auto *hint = mutedLabel(QStringLiteral("F11 fullscreen · Esc to return"), m_playerControls);
    controls->addWidget(hint);
    controls->addStretch();
    auto *restart = new QPushButton(QStringLiteral("Restart receiver"), m_playerControls);
    restart->setObjectName(QStringLiteral("secondaryButton"));
    connect(restart, &QPushButton::clicked, this, &MainWindow::restartReceiver);
    controls->addWidget(restart);
    m_fullscreenButton = new QPushButton(QStringLiteral("Fullscreen"), m_playerControls);
    m_fullscreenButton->setObjectName(QStringLiteral("fullscreenButton"));
    connect(m_fullscreenButton, &QPushButton::clicked, this, &MainWindow::enterFullscreen);
    controls->addWidget(m_fullscreenButton);
    m_playerLayout->addWidget(m_playerControls);
    m_playerPageLayout->addWidget(m_playerCard, 1);

    m_sessionPanel = card(m_playerPage);
    m_sessionPanel->setFixedWidth(286);
    auto *sessionLayout = new QVBoxLayout(m_sessionPanel);
    sessionLayout->setContentsMargins(22, 22, 22, 22);
    sessionLayout->setSpacing(9);
    auto *sessionTitle = new QLabel(QStringLiteral("Session"), m_sessionPanel);
    sessionTitle->setObjectName(QStringLiteral("cardTitle"));
    sessionLayout->addWidget(sessionTitle);
    m_sessionState = new QLabel(QStringLiteral("Receiver stopped"), m_sessionPanel);
    m_sessionState->setObjectName(QStringLiteral("sessionHero"));
    m_sessionState->setWordWrap(true);
    sessionLayout->addWidget(m_sessionState);
    sessionLayout->addSpacing(12);

    sessionLayout->addWidget(mutedLabel(QStringLiteral("DEVICE"), m_sessionPanel));
    m_deviceName = new QLabel(QStringLiteral("—"), m_sessionPanel);
    m_deviceName->setObjectName(QStringLiteral("valueLabel"));
    m_deviceName->setWordWrap(true);
    sessionLayout->addWidget(m_deviceName);
    m_deviceModel = mutedLabel(QStringLiteral("Waiting for a connection"), m_sessionPanel);
    sessionLayout->addWidget(m_deviceModel);
    sessionLayout->addSpacing(8);

    sessionLayout->addWidget(mutedLabel(QStringLiteral("STREAM"), m_sessionPanel));
    m_resolution = new QLabel(QStringLiteral("—"), m_sessionPanel);
    m_resolution->setObjectName(QStringLiteral("valueLabel"));
    sessionLayout->addWidget(m_resolution);
    m_duration = mutedLabel(QStringLiteral("00:00"), m_sessionPanel);
    sessionLayout->addWidget(m_duration);
    m_streamHealth = mutedLabel(QStringLiteral("Waiting for video"), m_sessionPanel);
    m_streamHealth->setObjectName(QStringLiteral("streamHealth"));
    m_streamHealth->setWordWrap(true);
    sessionLayout->addWidget(m_streamHealth);
    sessionLayout->addSpacing(8);

    sessionLayout->addWidget(mutedLabel(QStringLiteral("NETWORK"), m_sessionPanel));
    m_networkAddress = new QLabel(NetworkDiagnostics::primaryAddress(), m_sessionPanel);
    m_networkAddress->setObjectName(QStringLiteral("valueLabel"));
    sessionLayout->addWidget(m_networkAddress);
    m_securitySummary = mutedLabel({}, m_sessionPanel);
    sessionLayout->addWidget(m_securitySummary);
    sessionLayout->addStretch();

    m_reconnectButton = new QPushButton(QStringLiteral("Reconnect now"), m_sessionPanel);
    m_reconnectButton->setObjectName(QStringLiteral("secondaryButton"));
    m_reconnectButton->setToolTip(
        QStringLiteral("Restart the AirPlay receiver if the picture does not return."));
    connect(m_reconnectButton, &QPushButton::clicked, this, &MainWindow::restartReceiver);
    sessionLayout->addWidget(m_reconnectButton);

    auto *openSettings = new QPushButton(QStringLiteral("Open settings"), m_sessionPanel);
    openSettings->setObjectName(QStringLiteral("secondaryButton"));
    connect(openSettings, &QPushButton::clicked, this, [this]() { selectPage(2); });
    sessionLayout->addWidget(openSettings);
    m_playerPageLayout->addWidget(m_sessionPanel);
    return m_playerPage;
}

QWidget *MainWindow::createActivityPage() {
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 24, 30, 30);
    layout->setSpacing(14);

    auto *intro = mutedLabel(
        QStringLiteral("Connection events and recovery information stay local to this PC."), page);
    layout->addWidget(intro);
    auto *logCard = card(page);
    auto *logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(18, 18, 18, 18);
    auto *actions = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("Recent activity"), logCard);
    title->setObjectName(QStringLiteral("cardTitle"));
    actions->addWidget(title);
    actions->addStretch();
    auto *copy = new QPushButton(QStringLiteral("Copy"), logCard);
    copy->setObjectName(QStringLiteral("secondaryButton"));
    connect(copy, &QPushButton::clicked, this, [this]() {
        if (m_activityLog) m_activityLog->selectAll(), m_activityLog->copy();
    });
    actions->addWidget(copy);
    auto *clear = new QPushButton(QStringLiteral("Clear"), logCard);
    clear->setObjectName(QStringLiteral("secondaryButton"));
    connect(clear, &QPushButton::clicked, this, [this]() {
        if (m_activityLog) m_activityLog->clear();
    });
    actions->addWidget(clear);
    logLayout->addLayout(actions);
    m_activityLog = new QTextEdit(logCard);
    m_activityLog->setObjectName(QStringLiteral("activityLog"));
    m_activityLog->setReadOnly(true);
    m_activityLog->document()->setMaximumBlockCount(600);
    logLayout->addWidget(m_activityLog, 1);
    layout->addWidget(logCard, 1);
    return page;
}

QWidget *MainWindow::createSettingsPage() {
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("page"));
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setObjectName(QStringLiteral("settingsScroll"));
    auto *container = new QWidget(scroll);
    container->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(container);
    layout->setContentsMargins(30, 24, 30, 30);
    layout->setSpacing(16);

    auto *receiverCard = card(container);
    auto *receiverLayout = new QVBoxLayout(receiverCard);
    receiverLayout->setContentsMargins(22, 22, 22, 22);
    receiverLayout->addWidget(new QLabel(QStringLiteral("Receiver"), receiverCard));
    receiverLayout->addWidget(mutedLabel(
        QStringLiteral("This is the name shown in iPad Screen Mirroring."), receiverCard));
    m_receiverNameEdit = new QLineEdit(receiverCard);
    m_receiverNameEdit->setMaxLength(48);
    m_receiverNameEdit->setPlaceholderText(QStringLiteral("UxPlay Studio"));
    receiverLayout->addWidget(m_receiverNameEdit);
    receiverLayout->addSpacing(8);
    receiverLayout->addWidget(mutedLabel(QStringLiteral("QUALITY PROFILE"), receiverCard));
    m_qualityCombo = new QComboBox(receiverCard);
    m_qualityCombo->addItem(QStringLiteral("Low latency · 1080p 60 FPS · Recommended"),
                            static_cast<int>(QualityProfile::LowLatency1080p60));
    m_qualityCombo->addItem(QStringLiteral("Ultra low latency · 720p 30 FPS · Busy Wi-Fi"),
                            static_cast<int>(QualityProfile::UltraLowLatency720p30));
    m_qualityCombo->addItem(QStringLiteral("Balanced · 1080p 60 FPS"),
                            static_cast<int>(QualityProfile::Balanced1080p60));
    m_qualityCombo->addItem(QStringLiteral("Efficient · 720p 30 FPS"),
                            static_cast<int>(QualityProfile::Efficient720p30));
    receiverLayout->addWidget(m_qualityCombo);
    receiverLayout->addWidget(mutedLabel(
        QStringLiteral("Low-latency modes prioritize responsiveness. Balanced keeps audio and video synchronized for movies. Video always stays in the embedded D3D11 renderer."),
        receiverCard));
    layout->addWidget(receiverCard);

    auto *securityCard = card(container);
    auto *securityLayout = new QVBoxLayout(securityCard);
    securityLayout->setContentsMargins(22, 22, 22, 22);
    securityLayout->addWidget(new QLabel(QStringLiteral("Shared Wi-Fi protection"), securityCard));
    securityLayout->addWidget(mutedLabel(
        QStringLiteral("A PIN is recommended on dorm, office, or other shared networks."), securityCard));
    m_pinEnabledCheck = new QCheckBox(QStringLiteral("Require a four-digit AirPlay PIN"), securityCard);
    securityLayout->addWidget(m_pinEnabledCheck);
    m_pinEdit = new QLineEdit(securityCard);
    m_pinEdit->setMaxLength(4);
    m_pinEdit->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("[0-9]{4}")), m_pinEdit));
    m_pinEdit->setPlaceholderText(QStringLiteral("2468"));
    securityLayout->addWidget(m_pinEdit);
    connect(m_pinEnabledCheck, &QCheckBox::toggled, m_pinEdit, &QWidget::setEnabled);
    layout->addWidget(securityCard);

    auto *appCard = card(container);
    auto *appLayout = new QVBoxLayout(appCard);
    appLayout->setContentsMargins(22, 22, 22, 22);
    appLayout->addWidget(new QLabel(QStringLiteral("App behavior"), appCard));
    m_bluetoothCheck = new QCheckBox(QStringLiteral("Enable Bluetooth discovery fallback"), appCard);
    m_autostartCheck = new QCheckBox(QStringLiteral("Start UxPlay Studio when I sign in"), appCard);
    m_notificationsCheck = new QCheckBox(QStringLiteral("Show connection notifications"), appCard);
    appLayout->addWidget(m_bluetoothCheck);
    appLayout->addWidget(m_autostartCheck);
    appLayout->addWidget(m_notificationsCheck);
    layout->addWidget(appCard);

    auto *saveRow = new QHBoxLayout();
    m_settingsFeedback = mutedLabel({}, container);
    saveRow->addWidget(m_settingsFeedback, 1);
    auto *save = new QPushButton(QStringLiteral("Save & restart receiver"), container);
    save->setObjectName(QStringLiteral("primaryButton"));
    connect(save, &QPushButton::clicked, this, &MainWindow::saveSettings);
    saveRow->addWidget(save);
    layout->addLayout(saveRow);
    layout->addStretch();
    scroll->setWidget(container);
    outer->addWidget(scroll);
    return page;
}

QWidget *MainWindow::createDiagnosticsPage() {
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 24, 30, 30);
    layout->setSpacing(14);
    auto *intro = mutedLabel(
        QStringLiteral("Use this report when discovery or rendering does not work. It contains no passwords."), page);
    layout->addWidget(intro);
    auto *diagnosticCard = card(page);
    auto *diagnosticLayout = new QVBoxLayout(diagnosticCard);
    diagnosticLayout->setContentsMargins(18, 18, 18, 18);
    auto *row = new QHBoxLayout();
    auto *title = new QLabel(QStringLiteral("System report"), diagnosticCard);
    title->setObjectName(QStringLiteral("cardTitle"));
    row->addWidget(title);
    row->addStretch();
    auto *refresh = new QPushButton(QStringLiteral("Refresh"), diagnosticCard);
    refresh->setObjectName(QStringLiteral("secondaryButton"));
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDiagnostics);
    row->addWidget(refresh);
    auto *copy = new QPushButton(QStringLiteral("Copy report"), diagnosticCard);
    copy->setObjectName(QStringLiteral("secondaryButton"));
    connect(copy, &QPushButton::clicked, this, [this]() {
        if (m_diagnostics) m_diagnostics->selectAll(), m_diagnostics->copy();
    });
    row->addWidget(copy);
    diagnosticLayout->addLayout(row);
    m_diagnostics = new QPlainTextEdit(diagnosticCard);
    m_diagnostics->setReadOnly(true);
    m_diagnostics->setObjectName(QStringLiteral("diagnosticsText"));
    diagnosticLayout->addWidget(m_diagnostics, 1);
    layout->addWidget(diagnosticCard, 1);
    return page;
}

void MainWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }
    m_tray = new QSystemTrayIcon(windowIcon(), this);
    if (m_tray->icon().isNull()) {
        m_tray->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
    }
    m_tray->setToolTip(QStringLiteral("UxPlay Studio"));
    auto *menu = new QMenu(this);
    menu->addAction(QStringLiteral("Open UxPlay Studio"), this, &MainWindow::showFromTray);
    m_trayReceiverAction = menu->addAction(QStringLiteral("Start receiver"), this,
                                            &MainWindow::toggleReceiver);
    menu->addAction(QStringLiteral("Restart receiver"), this, &MainWindow::restartReceiver);
    menu->addSeparator();
    menu->addAction(QStringLiteral("Quit"), this, &MainWindow::quitApplication);
    m_tray->setContextMenu(menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger ||
                    reason == QSystemTrayIcon::DoubleClick) {
                    showFromTray();
                }
            });
    m_tray->show();
}

void MainWindow::startReceiver() {
    if (m_engine->isRunning()) {
        return;
    }
    if (!ensureBonjourAvailable()) {
        handleStateChanged(ReceiverState::Error);
        m_videoSurface->setPlaceholderText(
            QStringLiteral("Bonjour is not available"),
            QStringLiteral("Open Diagnostics for discovery details, then start the receiver again."));
        appendActivity(QStringLiteral("Error"),
                       QStringLiteral("Bonjour Service is required for AirPlay discovery."));
        return;
    }
    startBluetoothBeacon();
    appendActivity(QStringLiteral("Receiver"), QStringLiteral("Starting receiver…"));
    m_engine->start(m_config, m_videoSurface->nativeHandle(), bleStatusPath());
}

void MainWindow::stopReceiver() {
    stopBluetoothBeacon();
    appendActivity(QStringLiteral("Receiver"), QStringLiteral("Stopping receiver…"));
    m_engine->stop();
}

void MainWindow::restartReceiver() {
    if (!ensureBonjourAvailable()) {
        return;
    }
    stopBluetoothBeacon();
    startBluetoothBeacon();
    appendActivity(QStringLiteral("Receiver"), QStringLiteral("Restarting receiver…"));
    m_engine->restart(m_config, m_videoSurface->nativeHandle(), bleStatusPath());
}

void MainWindow::toggleReceiver() {
    if (m_engine->isRunning()) {
        stopReceiver();
    } else {
        startReceiver();
    }
}

void MainWindow::handleStateChanged(ReceiverState state) {
    const QString color = receiverStateColor(state);
    m_statusBadge->setText(receiverStateLabel(state));
    m_statusBadge->setStyleSheet(QStringLiteral(
        "QLabel { color: %1; background: %1; background-color: rgba(255,255,255,0.06);"
        " border: 1px solid %1; border-radius: 12px; padding: 5px 10px; }").arg(color));
    const bool running = state != ReceiverState::Stopped;
    m_reconnectButton->setEnabled(running);
    m_receiverToggle->setText(running ? QStringLiteral("Stop receiver")
                                      : QStringLiteral("Start receiver"));
    if (m_trayReceiverAction) {
        m_trayReceiverAction->setText(running ? QStringLiteral("Stop receiver")
                                              : QStringLiteral("Start receiver"));
    }

    m_sessionState->setText(receiverStateLabel(state));
    m_streamHealthMonitor.setMirroring(state == ReceiverState::Mirroring,
                                      m_healthClock.elapsed());
    if (state != ReceiverState::Mirroring) {
        m_videoSurface->setStreaming(false);
    }
    switch (state) {
    case ReceiverState::Stopped:
        m_videoSurface->setPlaceholderText(QStringLiteral("Receiver is stopped"),
                                           QStringLiteral("Select Start receiver when you are ready."));
        break;
    case ReceiverState::Starting:
    case ReceiverState::Retrying:
        m_videoSurface->setPlaceholderText(QStringLiteral("Starting the receiver…"),
                                           QStringLiteral("Preparing AirPlay discovery and the embedded renderer."));
        break;
    case ReceiverState::Ready:
        m_videoSurface->setPlaceholderText(
            QStringLiteral("Ready to mirror"),
            QStringLiteral("On iPad, open Control Center → Screen Mirroring → %1").arg(m_config.receiverName));
        break;
    case ReceiverState::Connecting:
        m_videoSurface->setPlaceholderText(QStringLiteral("Device is connecting…"),
                                           QStringLiteral("The stream will appear here automatically."));
        break;
    case ReceiverState::Mirroring:
        m_videoSurface->setStreaming(false);
        m_videoSurface->setPlaceholderText(
            QStringLiteral("Screen sharing connected"),
            QStringLiteral("Waiting for the first decoded video frame…"));
        if (!m_sessionElapsed.isValid()) m_sessionElapsed.start();
        break;
    case ReceiverState::Error:
        m_videoSurface->setPlaceholderText(QStringLiteral("Receiver needs attention"),
                                           QStringLiteral("Open Diagnostics or restart the receiver."));
        break;
    }
    updateStreamHealth();
}

void MainWindow::handleReceiverEvent(const ReceiverEvent &event) {
    const auto type = static_cast<uxplay_event_type>(event.type);
    QString category = QStringLiteral("Receiver");
    if (type == UXPLAY_EVENT_CLIENT_CONNECTING) {
        category = QStringLiteral("Connection");
        if (!event.deviceName.isEmpty()) m_deviceName->setText(event.deviceName);
        if (!event.deviceModel.isEmpty()) m_deviceModel->setText(event.deviceModel);
    } else if (type == UXPLAY_EVENT_MIRRORING_STARTED) {
        category = QStringLiteral("Stream");
        m_sessionElapsed.restart();
        m_resolution->setText(event.width > 0 && event.height > 0
                                  ? QStringLiteral("%1 × %2").arg(event.width).arg(event.height)
                                  : m_config.qualityLabel());
        if (m_config.notifications && m_tray) {
            m_tray->showMessage(QStringLiteral("Screen sharing started"),
                                QStringLiteral("%1 is now mirroring inside UxPlay Studio.")
                                    .arg(m_deviceName->text()),
                                QSystemTrayIcon::Information, 3000);
        }
    } else if (type == UXPLAY_EVENT_STREAM_STOPPED) {
        category = QStringLiteral("Stream");
        m_sessionElapsed.invalidate();
        m_duration->setText(QStringLiteral("00:00"));
        m_resolution->setText(QStringLiteral("—"));
    } else if (type == UXPLAY_EVENT_PIN_REQUIRED) {
        category = QStringLiteral("Security");
        m_sessionState->setText(QStringLiteral("Enter PIN %1 on your device").arg(event.message));
    } else if (type == UXPLAY_EVENT_WARNING) {
        category = QStringLiteral("Warning");
    } else if (type == UXPLAY_EVENT_ERROR) {
        category = QStringLiteral("Error");
    }
    appendActivity(category, event.message.isEmpty() ? receiverStateLabel(m_engine->state())
                                                      : event.message);
}

void MainWindow::handleVideoFrameDecoded() {
    if (m_engine->state() != ReceiverState::Mirroring) {
        return;
    }
    const auto before = m_streamHealthMonitor.health(m_healthClock.elapsed());
    m_streamHealthMonitor.frameReceived(m_healthClock.elapsed());
    const auto after = m_streamHealthMonitor.health(m_healthClock.elapsed());
    if (after == StreamHealthMonitor::Health::Live &&
        before != StreamHealthMonitor::Health::Live) {
        m_videoSurface->setStreaming(true);
        if (before == StreamHealthMonitor::Health::Restoring) {
            appendActivity(QStringLiteral("Recovery"),
                           QStringLiteral("Video frames resumed after Windows was unlocked."));
        }
    }
    updateStreamHealth();
}

void MainWindow::handleSessionLocked() {
    if (m_engine->state() != ReceiverState::Mirroring ||
        m_streamHealthMonitor.health(m_healthClock.elapsed()) ==
            StreamHealthMonitor::Health::Locked) {
        return;
    }
    m_streamHealthMonitor.sessionLocked(m_healthClock.elapsed());
    m_videoSurface->setStreaming(false);
    m_videoSurface->setPlaceholderText(
        QStringLiteral("PC session locked"),
        QStringLiteral("Video output is paused. UxPlay Studio will restore it after unlock."));
    appendActivity(QStringLiteral("Recovery"),
                   QStringLiteral("Windows was locked; video output is paused."));
    updateStreamHealth();
}

void MainWindow::handleSessionResumed() {
    const auto action = m_streamHealthMonitor.sessionResumed(m_healthClock.elapsed());
    if (action == StreamHealthMonitor::Action::None) {
        return;
    }
    m_videoSurface->setStreaming(false);
    m_videoSurface->setPlaceholderText(
        QStringLiteral("Restoring video output…"),
        QStringLiteral("The AirPlay session is still connected. Reattaching the renderer now."));
    appendActivity(QStringLiteral("Recovery"),
                   QStringLiteral("Windows resumed; restoring the embedded video output."));
    updateStreamHealth();
    performRecoveryAction(action);
}

void MainWindow::performRecoveryAction(StreamHealthMonitor::Action action) {
    if (action == StreamHealthMonitor::Action::RefreshRenderer) {
        if (!m_engine->recoverVideoOutput()) {
            m_streamHealthMonitor.rendererRefreshFailed();
            appendActivity(QStringLiteral("Recovery"),
                           QStringLiteral("Renderer refresh failed; restarting the receiver once."));
            updateStreamHealth();
            QTimer::singleShot(0, this, &MainWindow::restartReceiver);
        }
        return;
    }
    if (action == StreamHealthMonitor::Action::RestartReceiver) {
        m_videoSurface->setStreaming(false);
        m_videoSurface->setPlaceholderText(
            QStringLiteral("Reconnecting the receiver…"),
            QStringLiteral("Video did not resume after unlock. Restarting AirPlay once."));
        appendActivity(QStringLiteral("Recovery"),
                       QStringLiteral("No video frames returned after unlock; restarting the receiver once."));
        updateStreamHealth();
        restartReceiver();
    }
}

void MainWindow::updateStreamHealth() {
    if (!m_streamHealth) {
        return;
    }

    QString text;
    QString color = QStringLiteral("#8f9db5");
    switch (m_streamHealthMonitor.health(m_healthClock.elapsed())) {
    case StreamHealthMonitor::Health::Idle:
        text = m_engine->state() == ReceiverState::Ready
            ? QStringLiteral("Waiting for a device")
            : QStringLiteral("Video stream inactive");
        break;
    case StreamHealthMonitor::Health::WaitingForFrames:
        text = QStringLiteral("Connected · waiting for video frames…");
        color = QStringLiteral("#f6c85f");
        break;
    case StreamHealthMonitor::Health::Live:
        text = QStringLiteral("Video live · frames arriving");
        color = QStringLiteral("#38d996");
        break;
    case StreamHealthMonitor::Health::Locked:
        text = QStringLiteral("PC locked · video output paused");
        color = QStringLiteral("#f6c85f");
        break;
    case StreamHealthMonitor::Health::Restoring:
        text = QStringLiteral("Restoring video after unlock…");
        color = QStringLiteral("#6c8cff");
        break;
    case StreamHealthMonitor::Health::Reconnecting:
        text = QStringLiteral("Video did not recover · restarting receiver…");
        color = QStringLiteral("#ff9f5a");
        break;
    case StreamHealthMonitor::Health::Stalled: {
        const qint64 age = m_streamHealthMonitor.millisecondsSinceFrame(m_healthClock.elapsed());
        text = QStringLiteral("No new frames for %1s · device may be paused")
                   .arg(age < 0 ? 0 : age / 1000);
        color = QStringLiteral("#f6c85f");
        break;
    }
    }
    m_streamHealth->setText(text);
    m_streamHealth->setStyleSheet(QStringLiteral("color: %1;").arg(color));

    if (m_engine->state() == ReceiverState::Mirroring) {
        QString sessionText;
        QString badgeText;
        switch (m_streamHealthMonitor.health(m_healthClock.elapsed())) {
        case StreamHealthMonitor::Health::WaitingForFrames:
            sessionText = QStringLiteral("Starting video");
            badgeText = QStringLiteral("Starting video");
            break;
        case StreamHealthMonitor::Health::Live:
            sessionText = QStringLiteral("Screen sharing");
            badgeText = QStringLiteral("Screen sharing");
            break;
        case StreamHealthMonitor::Health::Locked:
            sessionText = QStringLiteral("Video paused while PC is locked");
            badgeText = QStringLiteral("Video paused");
            break;
        case StreamHealthMonitor::Health::Restoring:
            sessionText = QStringLiteral("Restoring video output");
            badgeText = QStringLiteral("Restoring video");
            break;
        case StreamHealthMonitor::Health::Reconnecting:
            sessionText = QStringLiteral("Reconnecting receiver");
            badgeText = QStringLiteral("Reconnecting");
            break;
        case StreamHealthMonitor::Health::Stalled:
            sessionText = QStringLiteral("Screen sharing paused");
            badgeText = QStringLiteral("Video paused");
            break;
        case StreamHealthMonitor::Health::Idle:
            break;
        }
        if (!sessionText.isEmpty()) {
            m_sessionState->setText(sessionText);
            m_statusBadge->setText(badgeText);
            m_statusBadge->setStyleSheet(QStringLiteral(
                "QLabel { color: %1; background: %1; background-color: rgba(255,255,255,0.06);"
                " border: 1px solid %1; border-radius: 12px; padding: 5px 10px; }")
                                             .arg(color));
        }
    }
}

void MainWindow::updateSessionTimer() {
    if (m_engine->state() == ReceiverState::Mirroring && m_sessionElapsed.isValid()) {
        m_duration->setText(formatDuration(m_sessionElapsed.elapsed() / 1000));
    }
    performRecoveryAction(m_streamHealthMonitor.tick(m_healthClock.elapsed()));
    updateStreamHealth();
}

void MainWindow::appendActivity(const QString &category, const QString &message) {
    if (!m_activityLog) {
        return;
    }
    const QString timestamp = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
    m_activityLog->append(QStringLiteral("%1  [%2]  %3").arg(timestamp, category, message));
}

ReceiverConfig MainWindow::configFromControls() const {
    ReceiverConfig config;
    config.receiverName = m_receiverNameEdit->text().trimmed();
    config.quality = static_cast<QualityProfile>(m_qualityCombo->currentData().toInt());
    config.pinEnabled = m_pinEnabledCheck->isChecked();
    config.pin = m_pinEdit->text();
    config.bluetoothDiscovery = m_bluetoothCheck->isChecked();
    config.autostart = m_autostartCheck->isChecked();
    config.notifications = m_notificationsCheck->isChecked();
    return config;
}

void MainWindow::loadConfigIntoControls() {
    m_receiverNameEdit->setText(m_config.receiverName);
    const int qualityIndex = m_qualityCombo->findData(static_cast<int>(m_config.quality));
    m_qualityCombo->setCurrentIndex(qualityIndex >= 0 ? qualityIndex : 0);
    m_pinEnabledCheck->setChecked(m_config.pinEnabled);
    m_pinEdit->setText(m_config.pin);
    m_pinEdit->setEnabled(m_config.pinEnabled);
    m_bluetoothCheck->setChecked(m_config.bluetoothDiscovery);
    m_autostartCheck->setChecked(m_config.autostart);
    m_notificationsCheck->setChecked(m_config.notifications);
    updateSecuritySummary();
}

void MainWindow::saveSettings() {
    const ReceiverConfig updated = configFromControls();
    const QString error = updated.validationError();
    if (!error.isEmpty()) {
        m_settingsFeedback->setText(error);
        m_settingsFeedback->setStyleSheet(QStringLiteral("color: #ff6b7a;"));
        return;
    }
    m_config = updated;
    SettingsStore::save(m_config);
    setAutostart(m_config.autostart);
    m_sidebarReceiver->setText(m_config.receiverName);
    updateSecuritySummary();
    refreshDiagnostics();
    m_settingsFeedback->setText(QStringLiteral("Saved. Receiver restarted with the new settings."));
    m_settingsFeedback->setStyleSheet(QStringLiteral("color: #38d996;"));
    appendActivity(QStringLiteral("Settings"), QStringLiteral("Receiver settings were updated."));
    if (m_engine->isRunning()) {
        restartReceiver();
    }
}

void MainWindow::updateSecuritySummary() {
    if (!m_securitySummary) {
        return;
    }
    m_securitySummary->setText(m_config.pinEnabled
        ? QStringLiteral("Protected by AirPlay PIN")
        : QStringLiteral("Open receiver · PIN recommended on shared Wi-Fi"));
    m_securitySummary->setStyleSheet(m_config.pinEnabled
        ? QStringLiteral("color: #38d996;") : QStringLiteral("color: #f6c85f;"));
}

void MainWindow::refreshDiagnostics() {
    if (!m_diagnostics || !m_videoSurface) {
        return;
    }
    m_networkAddress->setText(NetworkDiagnostics::primaryAddress());
    m_diagnostics->setPlainText(
        NetworkDiagnostics::report(m_config, m_videoSurface->nativeHandle()));
}

QString MainWindow::bleStatusPath() const {
    const QString directory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory);
    return QDir::toNativeSeparators(QDir(directory).filePath(QStringLiteral("uxplay_status.ble")));
}

void MainWindow::startBluetoothBeacon() {
    if (!m_config.bluetoothDiscovery || (m_beacon && m_beacon->state() != QProcess::NotRunning)) {
        return;
    }
    const QString executable = QDir(QApplication::applicationDirPath())
        .filePath(QStringLiteral("uxplay-bluetooth-beacon.exe"));
    if (!QFile::exists(executable)) {
        appendActivity(QStringLiteral("Bluetooth"),
                       QStringLiteral("Bluetooth helper is not present in this development build."));
        return;
    }
    m_beacon = new QProcess(this);
    m_beacon->setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_WIN
    m_beacon->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments *arguments) {
        arguments->flags |= CREATE_NO_WINDOW;
    });
#endif
    connect(m_beacon, &QProcess::readyRead, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_beacon->readAll()).trimmed();
        if (!text.isEmpty()) appendActivity(QStringLiteral("Bluetooth"), text);
    });
    m_beacon->start(executable, {QStringLiteral("--path"), bleStatusPath()});
}

void MainWindow::stopBluetoothBeacon() {
    if (!m_beacon) {
        return;
    }
    if (m_beacon->state() != QProcess::NotRunning) {
        m_beacon->terminate();
        if (!m_beacon->waitForFinished(800)) {
            m_beacon->kill();
            m_beacon->waitForFinished(200);
        }
    }
    delete m_beacon;
    m_beacon = nullptr;
}

bool MainWindow::ensureBonjourAvailable() {
    if (NetworkDiagnostics::bonjourServiceAvailable()) {
        return true;
    }
    const auto answer = QMessageBox::question(
        this, QStringLiteral("Bonjour Service required"),
        QStringLiteral("AirPlay discovery needs Bonjour Service. Install it now?"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        return false;
    }
    const int result = mdns::MdnsResponder::install();
    return result == 0 && NetworkDiagnostics::bonjourServiceAvailable();
}

bool MainWindow::autostartEnabled() const {
#ifdef Q_OS_WIN
    QSettings registry(QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    return registry.contains(QStringLiteral("UxPlayStudio"));
#else
    return false;
#endif
}

void MainWindow::setAutostart(bool enabled) {
#ifdef Q_OS_WIN
    QSettings registry(QStringLiteral(
        "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run"),
        QSettings::NativeFormat);
    if (enabled) {
        registry.setValue(QStringLiteral("UxPlayStudio"),
                          QStringLiteral("\"%1\"").arg(QDir::toNativeSeparators(
                              QApplication::applicationFilePath())));
    } else {
        registry.remove(QStringLiteral("UxPlayStudio"));
    }
#else
    Q_UNUSED(enabled)
#endif
}

void MainWindow::enterFullscreen() {
    if (m_fullscreen) return;
    selectPage(0);

    m_geometryBeforeFullscreen = saveGeometry();
    m_windowStateBeforeFullscreen = windowState() & ~Qt::WindowFullScreen;
    m_fullscreen = true;
    m_sidebar->hide();
    m_header->hide();
    m_sessionPanel->hide();
    m_playerChrome->hide();
    m_playerControls->hide();
    m_playerPageLayout->setContentsMargins(0, 0, 0, 0);
    m_playerPageLayout->setSpacing(0);
    m_playerLayout->setContentsMargins(0, 0, 0, 0);
    m_playerLayout->setSpacing(0);
    m_playerPage->setProperty("fullscreen", true);
    m_playerCard->setProperty("fullscreen", true);
    m_videoSurface->setProperty("fullscreen", true);
    refreshStyle(m_playerPage);
    refreshStyle(m_playerCard);
    refreshStyle(m_videoSurface);

    QApplication::setOverrideCursor(Qt::BlankCursor);
    m_cursorOverride = true;
    showFullScreen();
}

void MainWindow::exitFullscreen() {
    if (!m_fullscreen) return;
    m_fullscreen = false;

    if (m_cursorOverride) {
        QApplication::restoreOverrideCursor();
        m_cursorOverride = false;
    }
    m_playerPage->setProperty("fullscreen", false);
    m_playerCard->setProperty("fullscreen", false);
    m_videoSurface->setProperty("fullscreen", false);
    m_playerPageLayout->setContentsMargins(30, 24, 30, 30);
    m_playerPageLayout->setSpacing(20);
    m_playerLayout->setContentsMargins(18, 18, 18, 16);
    m_playerLayout->setSpacing(14);
    refreshStyle(m_playerPage);
    refreshStyle(m_playerCard);
    refreshStyle(m_videoSurface);

    m_sidebar->show();
    m_header->show();
    m_sessionPanel->show();
    m_playerChrome->show();
    m_playerControls->show();
    m_fullscreenButton->setText(QStringLiteral("Fullscreen"));

    if (m_windowStateBeforeFullscreen.testFlag(Qt::WindowMaximized)) {
        showMaximized();
    } else {
        showNormal();
        if (!m_geometryBeforeFullscreen.isEmpty()) {
            restoreGeometry(m_geometryBeforeFullscreen);
        }
    }
}

void MainWindow::showFromTray() {
    if (m_fullscreen) {
        exitFullscreen();
    } else {
        showNormal();
    }
    raise();
    activateWindow();
}

void MainWindow::quitApplication() {
    m_quitting = true;
    stopBluetoothBeacon();
    m_engine->stop();
    QApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_quitting) {
        event->accept();
        return;
    }
    if (!m_tray) {
        m_quitting = true;
        event->accept();
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
        return;
    }
    hide();
    event->ignore();
    if (!m_closeHintShown) {
        m_tray->showMessage(QStringLiteral("UxPlay Studio is still ready"),
                            QStringLiteral("Use the tray icon to reopen or quit the app."),
                            QSystemTrayIcon::Information, 3000);
        m_closeHintShown = true;
    }
}

void MainWindow::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_F11) {
        m_fullscreen ? exitFullscreen() : enterFullscreen();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_fullscreen) {
        exitFullscreen();
        event->accept();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
#ifdef Q_OS_WIN
    auto *nativeMessage = static_cast<MSG *>(message);
    if (nativeMessage && nativeMessage->message == WM_WTSSESSION_CHANGE) {
        if (nativeMessage->wParam == WTS_SESSION_LOCK) {
            handleSessionLocked();
        } else if (nativeMessage->wParam == WTS_SESSION_UNLOCK) {
            handleSessionResumed();
        }
    } else if (nativeMessage && nativeMessage->message == WM_POWERBROADCAST &&
               (nativeMessage->wParam == PBT_APMRESUMEAUTOMATIC ||
                nativeMessage->wParam == PBT_APMRESUMESUSPEND)) {
        handleSessionResumed();
    }
#else
    Q_UNUSED(eventType)
    Q_UNUSED(message)
    Q_UNUSED(result)
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}
