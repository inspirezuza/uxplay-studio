#include "mainwindow.h"

#include "airplayworker.h"
#include "mdns_responder.hpp"
#include "networkdiagnostics.h"
#include "projects/projectstore.h"
#include "recording/gstpipelinerunner.h"
#include "recording/recordingsession.h"
#include "receiverengine.h"
#include "export/exportjob.h"
#include "studio/scenecanvas.h"
#include "studio/scenedocument.h"
#include "videosurface.h"
#include "uxplay_api.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
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

    m_sceneDocument = std::make_unique<SceneDocument>();
    m_sceneDocument->setTitle(QStringLiteral("Untitled recording"));
    const QString airplay = m_sceneDocument->addSource(SceneSourceType::AirPlay,
                                                        QStringLiteral("iPad screen"));
    m_sceneDocument->addLayer(SceneFormat::Wide, airplay);
    m_sceneDocument->addLayer(SceneFormat::Vertical, airplay);
    QString projectRoot = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (projectRoot.isEmpty()) projectRoot = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    projectRoot = QDir(projectRoot).filePath(QStringLiteral("UxPlay Studio"));
    m_projectStore = std::make_unique<ProjectStore>(projectRoot);
    m_projectStore->recoverableProjects();
    m_pipelineRunner = std::make_unique<GstPipelineRunner>();
    m_recordingSession = new RecordingSession(m_projectStore.get(), m_pipelineRunner.get(), this);
    m_exportJob = new ExportJob(m_projectStore.get(), this);

    setupUi();
    setupTray();
    loadConfigIntoControls();

    connect(m_recordingSession, &RecordingSession::stateChanged, this,
            [this](RecordingState state) {
        if (!m_recordButton || !m_recordingStatus) return;
        const bool active = state == RecordingState::Recording || state == RecordingState::Finalizing;
        m_recordButton->setText(active ? QStringLiteral("Stop recording") : QStringLiteral("Record"));
        m_recordButton->setProperty("recording", active);
        refreshStyle(m_recordButton);
        m_recordingStatus->setText(state == RecordingState::Recording ? QStringLiteral("● REC · independent tracks") :
                                   state == RecordingState::Finalizing ? QStringLiteral("Finalizing media safely…") :
                                   state == RecordingState::Failed ? QStringLiteral("Recording needs attention") :
                                   QStringLiteral("Ready to record"));
        refreshProjectList();
    });
    connect(m_recordingSession, &RecordingSession::warningRaised, this,
            [this](const QString &message) { appendActivity(QStringLiteral("Recording"), message); });
    connect(m_exportJob, &ExportJob::started, this, [this](const QString &) {
        if (m_recordingStatus) m_recordingStatus->setText(QStringLiteral("Exporting in background…"));
    });
    connect(m_exportJob, &ExportJob::finished, this, [this](const QString &path) {
        if (m_recordingStatus) m_recordingStatus->setText(QStringLiteral("Export ready · %1").arg(QFileInfo(path).fileName()));
        refreshProjectList();
    });
    connect(m_exportJob, &ExportJob::failed, this, [this](const QString &message) {
        if (m_recordingStatus) m_recordingStatus->setText(QStringLiteral("Export failed · %1").arg(message));
        refreshProjectList();
    });

    connect(m_engine, &ReceiverEngine::stateChanged,
            this, &MainWindow::handleStateChanged);
    connect(m_engine, &ReceiverEngine::eventReceived,
            this, &MainWindow::handleReceiverEvent);
    connect(m_engine, &ReceiverEngine::recoveryScheduled, this, [this](int delayMs) {
        appendActivity(QStringLiteral("Recovery"),
                       QStringLiteral("Receiver will retry in %1 second(s).")
                           .arg(delayMs / 1000));
    });

    auto *timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &MainWindow::updateSessionTimer);
    timer->start(1000);

    handleStateChanged(ReceiverState::Stopped);
    refreshDiagnostics();
    if (m_autoStart) {
        QTimer::singleShot(0, this, &MainWindow::startReceiver);
    }
}

MainWindow::~MainWindow() {
    m_quitting = true;
    if (m_cursorOverride) {
        QApplication::restoreOverrideCursor();
        m_cursorOverride = false;
    }
    stopBluetoothBeacon();
    delete m_recordingSession;
    m_recordingSession = nullptr;
    delete m_exportJob;
    m_exportJob = nullptr;
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

    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Studio"), 0));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Projects"), 1));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Activity"), 2));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Settings"), 3));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Diagnostics"), 4));
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
    m_pageTitle = new QLabel(QStringLiteral("Studio"), m_header);
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
    m_pages->addWidget(createProjectsPage());
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
        QStringLiteral("Studio"), QStringLiteral("Projects"), QStringLiteral("Activity"),
        QStringLiteral("Settings"), QStringLiteral("Diagnostics")
    };
    m_pages->setCurrentIndex(page);
    m_pageTitle->setText(titles.value(page));
    for (int index = 0; index < m_navigationButtons.size(); ++index) {
        m_navigationButtons[index]->setChecked(index == page);
    }
    if (page == 1) refreshProjectList();
    if (page == 4) {
        refreshDiagnostics();
    }
}

void MainWindow::setStudioMode(bool edit) {
    if (!m_previewStack) return;
    m_previewStack->setCurrentIndex(edit ? 1 : 0);
    m_liveModeButton->setChecked(!edit);
    m_editModeButton->setChecked(edit);
}

void MainWindow::setSceneFormat(bool vertical) {
    m_sceneFormat = static_cast<int>(vertical ? SceneFormat::Vertical : SceneFormat::Wide);
    m_wideButton->setChecked(!vertical);
    m_verticalButton->setChecked(vertical);
    m_sceneCanvas->setFormat(static_cast<SceneFormat>(m_sceneFormat));
    refreshLayerList();
    setStudioMode(true);
}

void MainWindow::addStudioSource(int type) {
    SceneSourceType sourceType = SceneSourceType::Camera;
    QString name = QStringLiteral("Presenter camera");
    QString uri;
    if (type == 2) {
        sourceType = SceneSourceType::Image;
        uri = QFileDialog::getOpenFileName(this, QStringLiteral("Add image"), {},
                                           QStringLiteral("Images (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (uri.isEmpty()) return;
        name = QFileInfo(uri).completeBaseName();
    } else if (type == 3) {
        sourceType = SceneSourceType::Text;
        bool ok = false;
        name = QInputDialog::getText(this, QStringLiteral("Add text"), QStringLiteral("Text"),
                                     QLineEdit::Normal, QStringLiteral("Title"), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
    } else if (type == 4) {
        sourceType = SceneSourceType::Color;
        name = QStringLiteral("Color background");
        const QColor color = QColorDialog::getColor(QColor(QStringLiteral("#24345c")), this,
                                                     QStringLiteral("Choose layer color"));
        if (!color.isValid()) return;
        uri = color.name(QColor::HexRgb);
    }
    const QString sourceId = m_sceneDocument->addSource(sourceType, name, uri);
    const QString wide = m_sceneDocument->addLayer(SceneFormat::Wide, sourceId);
    const QString vertical = m_sceneDocument->addLayer(SceneFormat::Vertical, sourceId);
    if (sourceType == SceneSourceType::Camera) {
        SceneTransform wideTransform = m_sceneDocument->layer(SceneFormat::Wide, wide)->transform;
        wideTransform.frame = QRectF(1480, 690, 360, 300);
        wideTransform.mask = SceneMask::RoundedRectangle;
        m_sceneDocument->setTransform(SceneFormat::Wide, wide, wideTransform);
        SceneTransform verticalTransform = m_sceneDocument->layer(SceneFormat::Vertical, vertical)->transform;
        verticalTransform.frame = QRectF(680, 1480, 320, 320);
        verticalTransform.mask = SceneMask::Circle;
        m_sceneDocument->setTransform(SceneFormat::Vertical, vertical, verticalTransform);
    }
    m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
    refreshLayerList();
    m_sceneCanvas->selectLayer(m_sceneFormat == static_cast<int>(SceneFormat::Wide) ? wide : vertical);
    setStudioMode(true);
    saveCurrentProject();
}

void MainWindow::refreshLayerList() {
    if (!m_sourceList || !m_layerList || !m_sceneDocument) return;
    {
        QSignalBlocker sourceBlocker(m_sourceList);
        m_sourceList->clear();
        for (const SceneSource &source : m_sceneDocument->sources()) {
            QString kind;
            switch (source.type) {
            case SceneSourceType::AirPlay: kind = QStringLiteral("AirPlay"); break;
            case SceneSourceType::Camera: kind = QStringLiteral("Camera"); break;
            case SceneSourceType::Image: kind = QStringLiteral("Image"); break;
            case SceneSourceType::Text: kind = QStringLiteral("Text"); break;
            case SceneSourceType::Color: kind = QStringLiteral("Color"); break;
            }
            auto *item = new QListWidgetItem(QStringLiteral("%1   %2").arg(kind, source.name), m_sourceList);
            item->setData(Qt::UserRole, source.id);
        }
    }
    QSignalBlocker layerBlocker(m_layerList);
    m_layerList->clear();
    const auto &layers = m_sceneDocument->composition(static_cast<SceneFormat>(m_sceneFormat)).layers;
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        auto *item = new QListWidgetItem((it->locked ? QStringLiteral("🔒  ") : QStringLiteral("◇  ")) + it->name,
                                         m_layerList);
        item->setData(Qt::UserRole, it->id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
        item->setCheckState(it->visible ? Qt::Checked : Qt::Unchecked);
        if (it->locked) item->setForeground(QColor(QStringLiteral("#71809a")));
    }
}

void MainWindow::refreshProjectList() {
    if (!m_projectList || !m_projectStore) return;
    QSignalBlocker blocker(m_projectList);
    m_projectList->clear();
    const auto projects = m_projectStore->projects();
    for (const ProjectSummary &project : projects) {
        const QString state = projectStateKey(project.state).toUpper();
        auto *item = new QListWidgetItem(QStringLiteral("%1\n%2  ·  %3")
            .arg(project.title.isEmpty() ? QStringLiteral("Untitled recording") : project.title,
                 project.updatedAtUtc.toLocalTime().toString(QStringLiteral("dd MMM yyyy  HH:mm")), state),
            m_projectList);
        item->setData(Qt::UserRole, project.directory);
        item->setSizeHint(QSize(0, 58));
        if (project.state == ProjectState::Recoverable) item->setForeground(QColor(QStringLiteral("#f6c85f")));
    }
}

void MainWindow::toggleRecording() {
    if (m_recordingSession->state() == RecordingState::Recording) {
        m_recordingSession->stop();
        return;
    }
    if (m_engine->state() != ReceiverState::Mirroring) {
        m_recordingStatus->setText(QStringLiteral("Connect an iPad before recording"));
        return;
    }
    setStudioMode(false);
    QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    m_sceneDocument->setTitle(QStringLiteral("Recording %1")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm"))));
    const auto created = m_projectStore->create(*m_sceneDocument);
    if (!created.ok()) { m_recordingStatus->setText(created.error); return; }
    m_currentProject = std::make_unique<ProjectInfo>(created.project);
    QWidget *target = m_videoSurface->findChild<QWidget *>(QStringLiteral("nativeVideoTarget"));
    if (!target) target = m_videoSurface;
    const qreal scale = target->devicePixelRatioF();
    const QPoint global = target->mapToGlobal(QPoint(0, 0));
    RecordingOptions options;
#ifdef Q_OS_WIN
    const HWND targetWindow = reinterpret_cast<HWND>(target->winId());
    RECT targetRect{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    const HMONITOR monitor = MonitorFromWindow(targetWindow, MONITOR_DEFAULTTONEAREST);
    if (GetWindowRect(targetWindow, &targetRect) && GetMonitorInfoW(monitor, &monitorInfo)) {
        options.monitorHandle = reinterpret_cast<quint64>(monitor);
        options.captureRect = QRect(targetRect.left - monitorInfo.rcMonitor.left,
                                    targetRect.top - monitorInfo.rcMonitor.top,
                                    targetRect.right - targetRect.left,
                                    targetRect.bottom - targetRect.top);
    } else
#endif
    options.captureRect = QRect(qRound(global.x() * scale), qRound(global.y() * scale),
                                qRound(target->width() * scale), qRound(target->height() * scale));
    options.camera = m_recordCamera->isChecked();
    options.microphone = m_recordMicrophone->isChecked();
    options.systemAudio = true;
    if (!m_recordingSession->start(*m_currentProject, options))
        m_recordingStatus->setText(m_recordingSession->lastError());
}

void MainWindow::exportCurrentProject() {
    if (!m_currentProject) {
        m_recordingStatus->setText(QStringLiteral("Record or open a project before exporting"));
        return;
    }
    if (m_recordingSession->state() == RecordingState::Recording) {
        m_recordingStatus->setText(QStringLiteral("Stop recording before export"));
        return;
    }
    saveCurrentProject();
    QDir().mkpath(m_currentProject->exportsDirectory());
    const QString format = m_sceneFormat == static_cast<int>(SceneFormat::Vertical)
        ? QStringLiteral("vertical") : QStringLiteral("wide");
    const QString output = QDir(m_currentProject->exportsDirectory()).filePath(
        QStringLiteral("%1-%2.mp4").arg(format,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    m_exportJob->start(*m_currentProject, *m_sceneDocument,
                       static_cast<SceneFormat>(m_sceneFormat), output);
}

void MainWindow::saveCurrentProject() {
    if (m_currentProject && m_sceneDocument)
        m_projectStore->save(*m_currentProject, *m_sceneDocument);
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
    auto *toolbar = new QHBoxLayout(m_playerChrome);
    toolbar->setContentsMargins(2, 0, 2, 0);
    toolbar->setSpacing(7);
    m_liveModeButton = new QPushButton(QStringLiteral("Live"), m_playerChrome);
    m_editModeButton = new QPushButton(QStringLiteral("Edit layout"), m_playerChrome);
    m_wideButton = new QPushButton(QStringLiteral("16:9 Wide"), m_playerChrome);
    m_verticalButton = new QPushButton(QStringLiteral("9:16 Vertical"), m_playerChrome);
    for (auto *button : {m_liveModeButton, m_editModeButton, m_wideButton, m_verticalButton}) {
        button->setObjectName(QStringLiteral("segmentedButton"));
        button->setCheckable(true);
        toolbar->addWidget(button);
    }
    m_liveModeButton->setChecked(true);
    m_wideButton->setChecked(true);
    connect(m_liveModeButton, &QPushButton::clicked, this, [this]() { setStudioMode(false); });
    connect(m_editModeButton, &QPushButton::clicked, this, [this]() { setStudioMode(true); });
    connect(m_wideButton, &QPushButton::clicked, this, [this]() { setSceneFormat(false); });
    connect(m_verticalButton, &QPushButton::clicked, this, [this]() { setSceneFormat(true); });
    toolbar->addStretch();
    auto *embedded = new QLabel(QStringLiteral("D3D11 · EMBEDDED"), m_playerChrome);
    embedded->setObjectName(QStringLiteral("miniBadge"));
    toolbar->addWidget(embedded);
    m_playerLayout->addWidget(m_playerChrome);

    m_previewStack = new QStackedWidget(m_playerCard);
    m_previewStack->setObjectName(QStringLiteral("previewStack"));
    m_videoSurface = new VideoSurface(m_previewStack);
    m_sceneCanvas = new SceneCanvas(m_previewStack);
    m_sceneCanvas->setDocument(m_sceneDocument.get(), SceneFormat::Wide);
    m_previewStack->addWidget(m_videoSurface);
    m_previewStack->addWidget(m_sceneCanvas);
    m_playerLayout->addWidget(m_previewStack, 1);

    m_playerControls = new QWidget(m_playerCard);
    m_playerControls->setObjectName(QStringLiteral("playerControls"));
    auto *controls = new QHBoxLayout(m_playerControls);
    controls->setContentsMargins(0, 0, 0, 0);
    controls->setSpacing(7);
    const QList<QPair<QString, std::function<void()>>> commands {
        {QStringLiteral("Undo"), [this]() { m_sceneCanvas->undoStack()->undo(); }},
        {QStringLiteral("Redo"), [this]() { m_sceneCanvas->undoStack()->redo(); }},
        {QStringLiteral("Fit"), [this]() { m_sceneCanvas->fitSelection(); }},
        {QStringLiteral("Center"), [this]() { m_sceneCanvas->centerSelection(); }},
        {QStringLiteral("Reset"), [this]() { m_sceneCanvas->resetSelection(); }}
    };
    for (const auto &command : commands) {
        auto *button = new QPushButton(command.first, m_playerControls);
        button->setObjectName(QStringLiteral("secondaryButton"));
        connect(button, &QPushButton::clicked, this, command.second);
        controls->addWidget(button);
    }
    controls->addWidget(mutedLabel(QStringLiteral("Drag · handles resize · Alt+drag crop · Ctrl disables snap"), m_playerControls));
    controls->addStretch();
    m_fullscreenButton = new QPushButton(QStringLiteral("Fullscreen"), m_playerControls);
    m_fullscreenButton->setObjectName(QStringLiteral("fullscreenButton"));
    connect(m_fullscreenButton, &QPushButton::clicked, this, &MainWindow::enterFullscreen);
    controls->addWidget(m_fullscreenButton);
    m_playerLayout->addWidget(m_playerControls);
    m_playerPageLayout->addWidget(m_playerCard, 1);

    m_sessionPanel = card(m_playerPage);
    m_sessionPanel->setObjectName(QStringLiteral("studioDock"));
    m_sessionPanel->setFixedWidth(316);
    auto *dock = new QVBoxLayout(m_sessionPanel);
    dock->setContentsMargins(14, 14, 14, 14);
    dock->setSpacing(7);
    auto addSection = [this, dock](const QString &text) {
        auto *label = new QLabel(text, m_sessionPanel);
        label->setObjectName(QStringLiteral("dockTitle"));
        dock->addWidget(label);
    };
    addSection(QStringLiteral("SOURCES"));
    m_sourceList = new QListWidget(m_sessionPanel);
    m_sourceList->setObjectName(QStringLiteral("sourceList"));
    m_sourceList->setMaximumHeight(96);
    dock->addWidget(m_sourceList);
    auto *sourceRow = new QHBoxLayout;
    const QList<QPair<QString, int>> additions{{QStringLiteral("Camera"), 1}, {QStringLiteral("Image"), 2},
                                               {QStringLiteral("Text"), 3}, {QStringLiteral("Color"), 4}};
    for (const auto &entry : additions) {
        auto *button = new QPushButton(QStringLiteral("+") + entry.first, m_sessionPanel);
        button->setObjectName(QStringLiteral("tinyButton"));
        connect(button, &QPushButton::clicked, this, [this, entry]() { addStudioSource(entry.second); });
        sourceRow->addWidget(button);
    }
    dock->addLayout(sourceRow);

    addSection(QStringLiteral("LAYERS"));
    m_layerList = new QListWidget(m_sessionPanel);
    m_layerList->setObjectName(QStringLiteral("layerList"));
    m_layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_layerList->setDragDropMode(QAbstractItemView::InternalMove);
    dock->addWidget(m_layerList, 1);
    auto *layerRow = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Up"), m_sessionPanel);
    auto *down = new QPushButton(QStringLiteral("Down"), m_sessionPanel);
    auto *lock = new QPushButton(QStringLiteral("Lock"), m_sessionPanel);
    auto *remove = new QPushButton(QStringLiteral("Remove"), m_sessionPanel);
    for (auto *button : {up, down, lock, remove}) { button->setObjectName(QStringLiteral("tinyButton")); layerRow->addWidget(button); }
    dock->addLayout(layerRow);
    auto *appearanceRow = new QHBoxLayout;
    auto *opacity = new QComboBox(m_sessionPanel);
    opacity->setObjectName(QStringLiteral("compactCombo"));
    opacity->addItem(QStringLiteral("Opacity 100%"), 1.0);
    opacity->addItem(QStringLiteral("Opacity 75%"), .75);
    opacity->addItem(QStringLiteral("Opacity 50%"), .5);
    auto *mask = new QComboBox(m_sessionPanel);
    mask->setObjectName(QStringLiteral("compactCombo"));
    mask->addItem(QStringLiteral("No mask"), static_cast<int>(SceneMask::None));
    mask->addItem(QStringLiteral("Rounded"), static_cast<int>(SceneMask::RoundedRectangle));
    mask->addItem(QStringLiteral("Circle"), static_cast<int>(SceneMask::Circle));
    appearanceRow->addWidget(opacity);
    appearanceRow->addWidget(mask);
    dock->addLayout(appearanceRow);

    addSection(QStringLiteral("RECORD"));
    m_recordCamera = new QCheckBox(QStringLiteral("Camera track"), m_sessionPanel);
    m_recordMicrophone = new QCheckBox(QStringLiteral("Microphone track"), m_sessionPanel);
    dock->addWidget(m_recordCamera);
    dock->addWidget(m_recordMicrophone);
    m_recordingStatus = mutedLabel(QStringLiteral("Ready to record"), m_sessionPanel);
    dock->addWidget(m_recordingStatus);
    auto *recordRow = new QHBoxLayout;
    m_recordButton = new QPushButton(QStringLiteral("Record"), m_sessionPanel);
    m_recordButton->setObjectName(QStringLiteral("recordButton"));
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    recordRow->addWidget(m_recordButton, 1);
    auto *exportButton = new QPushButton(QStringLiteral("Export MP4"), m_sessionPanel);
    exportButton->setObjectName(QStringLiteral("secondaryButton"));
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportCurrentProject);
    recordRow->addWidget(exportButton);
    dock->addLayout(recordRow);

    m_sessionState = new QLabel(QStringLiteral("Receiver stopped"), m_sessionPanel);
    m_sessionState->setObjectName(QStringLiteral("sessionMini"));
    m_deviceName = new QLabel(QStringLiteral("—"), m_sessionPanel);
    m_deviceModel = mutedLabel(QStringLiteral("Waiting for a connection"), m_sessionPanel);
    m_resolution = new QLabel(QStringLiteral("—"), m_sessionPanel);
    m_duration = mutedLabel(QStringLiteral("00:00"), m_sessionPanel);
    m_networkAddress = new QLabel(NetworkDiagnostics::primaryAddress(), m_sessionPanel);
    m_securitySummary = mutedLabel({}, m_sessionPanel);
    auto *streamRow = new QHBoxLayout;
    streamRow->addWidget(m_sessionState, 1);
    streamRow->addWidget(m_duration);
    dock->addLayout(streamRow);
    dock->addWidget(m_deviceName);
    dock->addWidget(m_resolution);
    dock->addWidget(m_networkAddress);
    dock->addWidget(m_deviceModel);
    dock->addWidget(m_securitySummary);
    m_playerPageLayout->addWidget(m_sessionPanel);

    connect(m_layerList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_sceneCanvas->clearLayerSelection();
        for (QListWidgetItem *item : m_layerList->selectedItems())
            m_sceneCanvas->selectLayer(item->data(Qt::UserRole).toString(), true);
    });
    connect(m_layerList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        const QString id = item->data(Qt::UserRole).toString();
        if (!id.isEmpty()) m_sceneDocument->setLayerVisible(static_cast<SceneFormat>(m_sceneFormat), id,
                                                             item->checkState() == Qt::Checked);
        m_sceneCanvas->refreshFromDocument();
        saveCurrentProject();
    });
    connect(m_layerList->model(), &QAbstractItemModel::rowsMoved, this, [this]() {
        const SceneFormat format = static_cast<SceneFormat>(m_sceneFormat);
        for (int row = m_layerList->count() - 1, index = 0; row >= 0; --row, ++index)
            m_sceneDocument->moveLayer(format, m_layerList->item(row)->data(Qt::UserRole).toString(), index);
        m_sceneCanvas->setDocument(m_sceneDocument.get(), format);
        saveCurrentProject();
    });
    connect(m_sceneCanvas, &SceneCanvas::layerSelectionChanged, this, [this](const QStringList &ids) {
        QSignalBlocker blocker(m_layerList);
        for (int i = 0; i < m_layerList->count(); ++i)
            m_layerList->item(i)->setSelected(ids.contains(m_layerList->item(i)->data(Qt::UserRole).toString()));
    });
    connect(m_sceneCanvas, &SceneCanvas::sceneChanged, this, &MainWindow::saveCurrentProject);
    connect(opacity, &QComboBox::currentIndexChanged, this, [this, opacity](int) {
        m_sceneCanvas->setSelectionOpacity(opacity->currentData().toDouble());
    });
    connect(mask, &QComboBox::currentIndexChanged, this, [this, mask](int) {
        m_sceneCanvas->setSelectionMask(static_cast<SceneMask>(mask->currentData().toInt()));
    });

    auto moveCurrent = [this](int delta) {
        if (auto *item = m_layerList->currentItem()) {
            const QString id = item->data(Qt::UserRole).toString();
            const auto &layers = m_sceneDocument->composition(static_cast<SceneFormat>(m_sceneFormat)).layers;
            for (int i = 0; i < layers.size(); ++i) if (layers.at(i).id == id)
                m_sceneDocument->moveLayer(static_cast<SceneFormat>(m_sceneFormat), id, qBound(0, i + delta, layers.size() - 1));
            m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
            refreshLayerList(); saveCurrentProject();
        }
    };
    connect(up, &QPushButton::clicked, this, [moveCurrent]() { moveCurrent(1); });
    connect(down, &QPushButton::clicked, this, [moveCurrent]() { moveCurrent(-1); });
    connect(lock, &QPushButton::clicked, this, [this]() {
        if (auto *item = m_layerList->currentItem()) {
            const QString id = item->data(Qt::UserRole).toString();
            if (const SceneLayer *layer = m_sceneDocument->layer(static_cast<SceneFormat>(m_sceneFormat), id))
                m_sceneDocument->setLayerLocked(static_cast<SceneFormat>(m_sceneFormat), id, !layer->locked);
            m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
            refreshLayerList(); saveCurrentProject();
        }
    });
    connect(remove, &QPushButton::clicked, this, [this]() {
        if (auto *item = m_layerList->currentItem()) {
            m_sceneDocument->removeLayer(static_cast<SceneFormat>(m_sceneFormat), item->data(Qt::UserRole).toString());
            m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
            refreshLayerList(); saveCurrentProject();
        }
    });
    refreshLayerList();
    return m_playerPage;
}

QWidget *MainWindow::createProjectsPage() {
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(30, 24, 30, 30);
    layout->setSpacing(14);
    auto *intro = mutedLabel(QStringLiteral(
        "Recordings stay local and editable. Interrupted sessions are marked Recoverable; finalized segments are never discarded."), page);
    layout->addWidget(intro);
    auto *projectCard = card(page);
    auto *projectLayout = new QVBoxLayout(projectCard);
    projectLayout->setContentsMargins(18, 18, 18, 18);
    auto *row = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Local projects"), projectCard);
    title->setObjectName(QStringLiteral("cardTitle"));
    row->addWidget(title);
    row->addStretch();
    auto *openFolder = new QPushButton(QStringLiteral("Open recordings folder"), projectCard);
    openFolder->setObjectName(QStringLiteral("secondaryButton"));
    connect(openFolder, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_projectStore->rootDirectory()));
    });
    row->addWidget(openFolder);
    projectLayout->addLayout(row);
    m_projectList = new QListWidget(projectCard);
    m_projectList->setObjectName(QStringLiteral("projectList"));
    projectLayout->addWidget(m_projectList, 1);
    auto *actions = new QHBoxLayout;
    auto *open = new QPushButton(QStringLiteral("Open in Studio"), projectCard);
    open->setObjectName(QStringLiteral("primaryButton"));
    connect(open, &QPushButton::clicked, this, [this]() {
        auto *item = m_projectList ? m_projectList->currentItem() : nullptr;
        if (!item) return;
        auto loaded = m_projectStore->load(item->data(Qt::UserRole).toString());
        if (!loaded.ok()) return;
        m_sceneDocument = std::move(loaded.document);
        m_currentProject = std::make_unique<ProjectInfo>(loaded.project);
        m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
        refreshLayerList();
        selectPage(0);
        setStudioMode(true);
    });
    actions->addWidget(open);
    auto *recover = new QPushButton(QStringLiteral("Recover session"), projectCard);
    recover->setObjectName(QStringLiteral("secondaryButton"));
    connect(recover, &QPushButton::clicked, this, [this, open]() {
        auto *item = m_projectList ? m_projectList->currentItem() : nullptr;
        if (!item) return;
        m_projectStore->setState(item->data(Qt::UserRole).toString(), ProjectState::Ready);
        refreshProjectList();
        open->click();
    });
    actions->addWidget(recover);
    actions->addStretch();
    projectLayout->addLayout(actions);
    layout->addWidget(projectCard, 1);
    refreshProjectList();
    return page;
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
    m_receiverToggle->setText(running ? QStringLiteral("Stop receiver")
                                      : QStringLiteral("Start receiver"));
    if (m_trayReceiverAction) {
        m_trayReceiverAction->setText(running ? QStringLiteral("Stop receiver")
                                              : QStringLiteral("Start receiver"));
    }

    m_sessionState->setText(receiverStateLabel(state));
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
        m_videoSurface->setStreaming(true);
        if (!m_sessionElapsed.isValid()) m_sessionElapsed.start();
        break;
    case ReceiverState::Error:
        m_videoSurface->setPlaceholderText(QStringLiteral("Receiver needs attention"),
                                           QStringLiteral("Open Diagnostics or restart the receiver."));
        break;
    }
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

void MainWindow::updateSessionTimer() {
    if (m_engine->state() == ReceiverState::Mirroring && m_sessionElapsed.isValid()) {
        m_duration->setText(formatDuration(m_sessionElapsed.elapsed() / 1000));
    }
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
    setStudioMode(false);

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
