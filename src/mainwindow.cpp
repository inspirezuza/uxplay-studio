#include "mainwindow.h"

#include "airplayworker.h"
#include "mdns_responder.hpp"
#include "networkdiagnostics.h"
#include "projects/projectstore.h"
#include "recording/gstpipelinerunner.h"
#include "recording/camerapreviewengine.h"
#include "recording/recordingsession.h"
#include "receiverengine.h"
#include "export/exportjob.h"
#include "studio/scenecanvas.h"
#include "studio/cameraselfview.h"
#include "studio/scenedocument.h"
#include "ui/studiovisuals.h"
#include "videosurface.h"
#include "uxplay_api.h"

#include <QAction>
#include <QAbstractItemModel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QImage>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
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
#include <QSplitter>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>
#include <utility>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

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

// QScrollArea normally advertises the full minimum size of its child.  That
// is useful in a standalone form, but in the nested layer splitter it would
// consume all available height and leave the layer list at its 72px minimum.
// The inspector can scroll, so its splitter-facing size hint should be
// intentionally zero; the splitter's user-controlled sizes then win.
class InspectorScrollArea final : public QScrollArea {
public:
    using QScrollArea::QScrollArea;

    QSize sizeHint() const override { return QSize(0, 0); }
    QSize minimumSizeHint() const override { return QSize(0, 0); }
};

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

struct VideoPreviewResult {
    QImage image;
    QString error;
};

VideoPreviewResult loadVideoPreview(const QString &path, const QString &label) {
    if (path.isEmpty() || !QFileInfo::exists(path)) return {};
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace('\\', QStringLiteral("\\\\"));
    escaped.replace('"', QStringLiteral("\\\""));
    const QString description = QStringLiteral(
        "filesrc location=\"%1\" ! decodebin ! videoconvert ! videoscale ! "
        "video/x-raw,format=BGRA,width=960 ! appsink name=preview sync=false max-buffers=1 drop=true")
        .arg(escaped);
    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
    if (!pipeline || error) {
        const QString message = error
            ? QString::fromUtf8(error->message)
            : QStringLiteral("the preview pipeline could not be created");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
        return {{}, QStringLiteral("Could not load the %1 preview: %2").arg(label, message)};
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "preview");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstSample *sample = sink ? gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND) : nullptr;
    QImage image;
    if (sample) {
        GstVideoInfo info;
        GstMapInfo map;
        GstCaps *caps = gst_sample_get_caps(sample);
        GstBuffer *buffer = gst_sample_get_buffer(sample);
        if (caps && buffer && gst_video_info_from_caps(&info, caps) &&
            gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            image = QImage(map.data, GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                           GST_VIDEO_INFO_PLANE_STRIDE(&info, 0), QImage::Format_ARGB32).copy();
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
    }
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink) gst_object_unref(sink);
    gst_object_unref(pipeline);
    if (!image.isNull()) return {image, {}};
    return {{}, QStringLiteral("Could not decode the %1 preview from %2")
                    .arg(label, QFileInfo(path).fileName())};
}

}

MainWindow::MainWindow(QWidget *parent, bool autoStart, const QString &projectRootOverride)
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
    QString projectRoot = projectRootOverride;
    if (projectRoot.isEmpty()) projectRoot = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (projectRoot.isEmpty()) projectRoot = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    projectRoot = QDir(projectRoot).filePath(QStringLiteral("UxPlay Studio"));
    m_projectStore = std::make_unique<ProjectStore>(projectRoot);
    m_projectStore->recoverableProjects();
    m_pipelineRunner = std::make_unique<GstPipelineRunner>();
    m_cameraPreviewEngine = new CameraPreviewEngine(this);
    connect(m_cameraPreviewEngine, &CameraPreviewEngine::frameReady,
            this, &MainWindow::handleCameraPreviewFrame);
    connect(m_cameraPreviewEngine, &CameraPreviewEngine::stoppedUnexpectedly,
            this, [this](const QString &message) {
        if (m_cameraSelfView) m_cameraSelfView->setActive(false);
        if (m_recordingStatus)
            m_recordingStatus->setText(QStringLiteral("Camera preview stopped · %1").arg(message));
        appendActivity(QStringLiteral("Camera"), message);
    });
    m_pipelineRunner->setCameraPreviewCallback([this](const QImage &frame) {
        QMetaObject::invokeMethod(this, [this, frame]() { handleCameraPreviewFrame(frame); },
                                  Qt::QueuedConnection);
    });
    m_recordingSession = new RecordingSession(m_projectStore.get(), m_pipelineRunner.get(), this);
    m_exportJob = new ExportJob(m_projectStore.get(), this);

    setupUi();
    setupTray();
    loadConfigIntoControls();

    connect(m_recordingSession, &RecordingSession::stateChanged, this,
            [this](RecordingState state) {
        const bool busy = state == RecordingState::Starting ||
                          state == RecordingState::Recording ||
                          state == RecordingState::Finalizing;
        setRecordingUiLocked(busy);
        if (!m_recordButton || !m_recordingStatus) return;
        const bool active = state == RecordingState::Recording ||
                            state == RecordingState::Finalizing ||
                            state == RecordingState::Starting;
        m_recordButton->setText(active ? QStringLiteral("Stop safely")
                                       : QStringLiteral("Start recording"));
        m_recordButton->setProperty("recording", active);
        refreshStyle(m_recordButton);
        m_recordingStatus->setText(m_recordingSession->statusSummary());
        if (active && m_statusBadge) {
            m_statusBadge->setText(state == RecordingState::Finalizing
                                       ? QStringLiteral("Finalizing recording")
                                       : QStringLiteral("Recording"));
            m_statusBadge->setProperty("tone", state == RecordingState::Finalizing
                                                   ? QStringLiteral("warning")
                                                   : QStringLiteral("live"));
            refreshStyle(m_statusBadge);
        } else if (m_engine) {
            handleStateChanged(m_engine->state());
        }
        refreshProjectList();
        if (state == RecordingState::Starting && m_recordCamera && m_recordCamera->isChecked())
            m_cameraPreviewEngine->stop();
        if ((state == RecordingState::Idle || state == RecordingState::Failed) &&
            m_recordCamera && m_recordCamera->isChecked())
            setCameraPreviewEnabled(true);
        if (state == RecordingState::Idle && m_currentProject)
            loadRecordedPreviews(*m_currentProject);
    });
    connect(m_recordingSession, &RecordingSession::warningRaised, this,
            [this](const QString &message) {
        appendActivity(QStringLiteral("Recording"), message);
        if (m_recordingStatus)
            m_recordingStatus->setText(m_recordingSession->statusSummary());
        if (message.contains(QStringLiteral("camera"), Qt::CaseInsensitive) && m_cameraSelfView)
            m_cameraSelfView->setActive(false);
    });
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
    connect(m_engine, &ReceiverEngine::previewFrame, this, [this](const QImage &frame) {
        if (!m_sceneCanvas || !m_sceneDocument) return;
        for (const SceneSource &source : m_sceneDocument->sources()) {
            if (source.type == SceneSourceType::AirPlay) {
                m_sceneCanvas->setSourcePreview(source.id, frame);
                break;
            }
        }
    });
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
    if (m_cameraPreviewEngine) m_cameraPreviewEngine->stop();
    if (m_cursorOverride) {
        QApplication::restoreOverrideCursor();
        m_cursorOverride = false;
    }
    stopBluetoothBeacon();
    for (QThread *thread : std::as_const(m_previewThreads)) {
        if (thread && thread->isRunning()) thread->requestInterruption();
    }
    for (QThread *thread : std::as_const(m_previewThreads)) {
        if (thread && thread->isRunning()) thread->wait();
    }
    delete m_recordingSession;
    m_recordingSession = nullptr;
    delete m_exportJob;
    m_exportJob = nullptr;
}

void MainWindow::setupUi() {
    setWindowTitle(QStringLiteral("UxPlay Studio"));
    setMinimumSize(1024, 680);
    resize(1440, 880);

    auto *central = new QWidget(this);
    central->setObjectName(QStringLiteral("appRoot"));
    setCentralWidget(central);
    auto *shell = new QHBoxLayout(central);
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);

    m_sidebar = new QWidget(central);
    m_sidebar->setObjectName(QStringLiteral("sidebar"));
    m_sidebar->setFixedWidth(112);
    auto *sidebarLayout = new QVBoxLayout(m_sidebar);
    sidebarLayout->setContentsMargins(12, 16, 12, 14);
    sidebarLayout->setSpacing(7);

    auto *brandMark = new QLabel(QStringLiteral("UX"), m_sidebar);
    brandMark->setObjectName(QStringLiteral("brandMark"));
    brandMark->setFixedSize(42, 42);
    sidebarLayout->addWidget(brandMark, 0, Qt::AlignHCenter);
    auto *brand = new QLabel(QStringLiteral("UXPLAY"), m_sidebar);
    brand->setObjectName(QStringLiteral("brand"));
    sidebarLayout->addWidget(brand);
    m_sidebarReceiver = mutedLabel(m_config.receiverName, m_sidebar);
    m_sidebarReceiver->setObjectName(QStringLiteral("sidebarReceiver"));
    sidebarLayout->addWidget(m_sidebarReceiver);
    sidebarLayout->addSpacing(10);

    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Studio"), 0));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Projects"), 1));
    sidebarLayout->addWidget(createNavigationButton(QStringLiteral("Settings"), 3));
    sidebarLayout->addStretch();

    auto *opensource = mutedLabel(QStringLiteral("LOCAL ONLY\nGPL-3.0"), m_sidebar);
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
    m_header->setFixedHeight(72);
    auto *headerLayout = new QHBoxLayout(m_header);
    headerLayout->setContentsMargins(22, 10, 22, 10);
    auto *headerCopy = new QWidget(m_header);
    auto *headerCopyLayout = new QVBoxLayout(headerCopy);
    headerCopyLayout->setContentsMargins(0, 0, 0, 0);
    headerCopyLayout->setSpacing(1);
    m_pageEyebrow = new QLabel(QStringLiteral("LIVE WORKSPACE"), headerCopy);
    m_pageEyebrow->setObjectName(QStringLiteral("pageEyebrow"));
    headerCopyLayout->addWidget(m_pageEyebrow);
    m_pageTitle = new QLabel(m_config.receiverName, headerCopy);
    m_pageTitle->setObjectName(QStringLiteral("pageTitle"));
    headerCopyLayout->addWidget(m_pageTitle);
    headerLayout->addWidget(headerCopy);
    headerLayout->addStretch();
    auto *localBadge = new QLabel(QStringLiteral("LOCAL ONLY"), m_header);
    localBadge->setObjectName(QStringLiteral("localBadge"));
    headerLayout->addWidget(localBadge);
    m_statusBadge = new QLabel(QStringLiteral("Stopped"), m_header);
    m_statusBadge->setObjectName(QStringLiteral("statusBadge"));
    headerLayout->addWidget(m_statusBadge);
    m_receiverToggle = new QPushButton(QStringLiteral("Start receiver"), m_header);
    m_receiverToggle->setObjectName(QStringLiteral("receiverButton"));
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
    button->setProperty("page", page);
    const QString icon = page == 0 ? QStringLiteral("studio")
        : page == 1 ? QStringLiteral("projects") : QStringLiteral("settings");
    button->setIcon(StudioVisuals::icon(icon));
    button->setIconSize(QSize(20, 20));
    connect(button, &QPushButton::clicked, this, [this, page]() { selectPage(page); });
    m_navigationButtons.append(button);
    return button;
}

void MainWindow::selectPage(int page) {
    if (!m_pages || page < 0 || page >= m_pages->count()) {
        return;
    }
    const QStringList titles {
        m_config.receiverName, QStringLiteral("Projects"), QStringLiteral("Activity"),
        QStringLiteral("Settings"), QStringLiteral("Diagnostics")
    };
    const QStringList eyebrows {
        QStringLiteral("LIVE WORKSPACE"), QStringLiteral("LOCAL LIBRARY"),
        QStringLiteral("SESSION HISTORY"), QStringLiteral("PREFERENCES"),
        QStringLiteral("ADVANCED SUPPORT")
    };
    m_pages->setCurrentIndex(page);
    m_pageTitle->setText(titles.value(page));
    if (m_pageEyebrow) m_pageEyebrow->setText(eyebrows.value(page));
    for (QPushButton *button : std::as_const(m_navigationButtons))
        button->setChecked(button->property("page").toInt() == page);
    if (page == 1) refreshProjectList();
    if (page == 4) {
        refreshDiagnostics();
    }
}

void MainWindow::setStudioMode(bool edit) {
    if (!m_previewStack) return;
    if (edit && m_recordingSession &&
        (m_recordingSession->state() == RecordingState::Starting ||
         m_recordingSession->state() == RecordingState::Recording ||
         m_recordingSession->state() == RecordingState::Finalizing)) {
        edit = false;
    }
    m_previewStack->setCurrentIndex(edit ? 1 : 0);
    // The camera self-view is a live-mode monitor. In Edit layout the camera
    // is already rendered by its scene layer, so leaving this native overlay
    // visible would show the same feed twice.
    if (m_cameraSelfView) {
        m_cameraSelfView->setOverlayVisible(!edit);
        if (!edit) m_cameraSelfView->setActive(m_cameraSelfView->isActive());
    }
    m_liveModeButton->setChecked(!edit);
    m_editModeButton->setChecked(edit);
    for (QPushButton *button : std::as_const(m_editActionButtons))
        button->setVisible(edit);
    if (m_canvasViewControls) m_canvasViewControls->setVisible(edit);
    if (m_recordCamera) m_recordCamera->setVisible(!edit);
    if (m_recordMicrophone) m_recordMicrophone->setVisible(!edit);
    if (m_duration) m_duration->setVisible(!edit);
    if (m_recordButton) m_recordButton->setVisible(!edit);
    if (m_transformPanel) m_transformPanel->setVisible(edit);
}

void MainWindow::deleteSelectedLayers() {
    if (!m_sceneCanvas || !m_pages || m_pages->currentIndex() != 0 ||
        !m_previewStack || m_previewStack->currentIndex() != 1) {
        return;
    }
    QWidget *focus = QApplication::focusWidget();
    if (qobject_cast<QLineEdit *>(focus) || qobject_cast<QTextEdit *>(focus) ||
        qobject_cast<QPlainTextEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus) ||
        qobject_cast<QComboBox *>(focus)) {
        return;
    }
    if (!m_sceneCanvas->deleteSelection() && m_recordingStatus) {
        m_recordingStatus->setText(QStringLiteral("Select an unlocked layer to delete"));
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ContextMenu &&
        (watched == m_stageFrame || watched == m_previewStack || watched == m_playerCard)) {
        auto *context = static_cast<QContextMenuEvent *>(event);
        showCanvasContextMenu({}, context->globalPos());
        return true;
    }
    if (m_layerList && (watched == m_layerList || watched == m_layerList->viewport()) &&
        event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Delete || key->key() == Qt::Key_Backspace) {
            deleteSelectedLayers();
            return true;
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::setRecordingUiLocked(bool locked) {
    for (QPushButton *button : std::as_const(m_navigationButtons))
        button->setEnabled(!locked);
    for (QPushButton *button : {m_editModeButton, m_wideButton, m_verticalButton})
        if (button) button->setEnabled(!locked);
    if (m_sourceList) m_sourceList->setEnabled(!locked);
    if (m_layerList) m_layerList->setEnabled(!locked);
    if (m_recordCamera) m_recordCamera->setEnabled(!locked);
    if (m_recordMicrophone) m_recordMicrophone->setEnabled(!locked);
    for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("sourceButton")))
        button->setEnabled(!locked);
    for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("iconButton")))
        button->setEnabled(!locked);
}

void MainWindow::setSceneFormat(bool vertical) {
    if (m_recordingSession &&
        (m_recordingSession->state() == RecordingState::Starting ||
         m_recordingSession->state() == RecordingState::Recording ||
         m_recordingSession->state() == RecordingState::Finalizing)) {
        return;
    }
    m_sceneFormat = static_cast<int>(vertical ? SceneFormat::Vertical : SceneFormat::Wide);
    m_wideButton->setChecked(!vertical);
    m_verticalButton->setChecked(vertical);
    m_sceneCanvas->setFormat(static_cast<SceneFormat>(m_sceneFormat));
    refreshLayerList();
    updateCanvasHint();
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
        m_recordCamera->setChecked(true);
        setCameraPreviewEnabled(true);
    }
    m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
    refreshLayerList();
    m_sceneCanvas->selectLayer(m_sceneFormat == static_cast<int>(SceneFormat::Wide) ? wide : vertical);
    setStudioMode(true);
    saveCurrentProject();
}

void MainWindow::refreshLayerList() {
    if (!m_sourceList || !m_layerList || !m_sceneDocument) return;
    const auto sourceStatus = [this](SceneSourceType type) {
        if (type == SceneSourceType::AirPlay && m_engine) {
            switch (m_engine->state()) {
            case ReceiverState::Stopped: return QStringLiteral("OFFLINE");
            case ReceiverState::Starting: return QStringLiteral("STARTING");
            case ReceiverState::Ready: return QStringLiteral("READY");
            case ReceiverState::Connecting: return QStringLiteral("CONNECTING");
            case ReceiverState::Mirroring: return QStringLiteral("LIVE");
            case ReceiverState::Error: return QStringLiteral("ERROR");
            case ReceiverState::Retrying: return QStringLiteral("RECONNECTING");
            }
        }
        if (type == SceneSourceType::Camera && m_cameraPreviewEngine &&
            m_cameraPreviewEngine->isRunning()) {
            return QStringLiteral("PREVIEW");
        }
        return QStringLiteral("READY");
    };
    {
        QSignalBlocker sourceBlocker(m_sourceList);
        m_sourceList->clear();
        for (const SceneSource &source : m_sceneDocument->sources()) {
            QString kind;
            QString icon;
            switch (source.type) {
            case SceneSourceType::AirPlay: kind = QStringLiteral("AIRPLAY"); icon = QStringLiteral("airplay"); break;
            case SceneSourceType::Camera: kind = QStringLiteral("CAMERA"); icon = QStringLiteral("camera"); break;
            case SceneSourceType::Image: kind = QStringLiteral("IMAGE"); icon = QStringLiteral("image"); break;
            case SceneSourceType::Text: kind = QStringLiteral("TEXT"); icon = QStringLiteral("text"); break;
            case SceneSourceType::Color: kind = QStringLiteral("COLOR"); icon = QStringLiteral("color"); break;
            }
            auto *item = new QListWidgetItem(StudioVisuals::icon(icon),
                QStringLiteral("%1\n%2 · %3").arg(source.name, kind, sourceStatus(source.type)),
                m_sourceList);
            item->setData(Qt::UserRole, source.id);
            item->setSizeHint(QSize(0, 54));
        }
        if (m_sourceCount) {
            const int count = m_sceneDocument->sources().size();
            m_sourceCount->setText(QStringLiteral("%1 %2").arg(count).arg(count == 1 ? QStringLiteral("source")
                                                                                     : QStringLiteral("sources")));
        }
    }
    QSignalBlocker layerBlocker(m_layerList);
    m_layerList->clear();
    const auto &layers = m_sceneDocument->composition(static_cast<SceneFormat>(m_sceneFormat)).layers;
    for (auto it = layers.crbegin(); it != layers.crend(); ++it) {
        auto *item = new QListWidgetItem((it->locked ? QStringLiteral("🔒  ") : QStringLiteral("◇  ")) + it->name,
                                         m_layerList);
        item->setIcon(StudioVisuals::icon(QStringLiteral("studio")));
        item->setData(Qt::UserRole, it->id);
        item->setFlags(item->flags() | Qt::ItemIsDragEnabled);
        if (!it->locked) item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(it->visible ? Qt::Checked : Qt::Unchecked);
        if (it->locked) item->setForeground(QColor(QStringLiteral("#71809a")));
    }
    refreshLayerInspector();
}

void MainWindow::refreshLayerInspector() {
    if (!m_layerList || !m_sceneDocument || !m_layerX || !m_layerY ||
        !m_layerWidth || !m_layerHeight || !m_layerRotation) return;
    const auto selected = m_layerList->selectedItems();
    const bool oneLayer = selected.size() == 1;
    for (QDoubleSpinBox *field : {m_layerX, m_layerY, m_layerWidth,
                                  m_layerHeight, m_layerRotation})
        field->setEnabled(oneLayer);
    if (m_layerOpacity) m_layerOpacity->setEnabled(oneLayer);
    if (m_layerMask) m_layerMask->setEnabled(oneLayer);
    if (!oneLayer) return;
    const SceneLayer *layer = m_sceneDocument->layer(
        static_cast<SceneFormat>(m_sceneFormat), selected.first()->data(Qt::UserRole).toString());
    if (!layer) return;
    m_updatingInspector = true;
    const QList<QPair<QDoubleSpinBox *, qreal>> values {
        {m_layerX, layer->transform.frame.x()},
        {m_layerY, layer->transform.frame.y()},
        {m_layerWidth, layer->transform.frame.width()},
        {m_layerHeight, layer->transform.frame.height()},
        {m_layerRotation, layer->transform.rotationDegrees}
    };
    for (const auto &value : values) {
        QSignalBlocker blocker(value.first);
        value.first->setValue(value.second);
    }
    if (m_layerOpacity) {
        QSignalBlocker blocker(m_layerOpacity);
        const int index = m_layerOpacity->findData(layer->transform.opacity);
        m_layerOpacity->setCurrentIndex(index >= 0 ? index : 0);
    }
    if (m_layerMask) {
        QSignalBlocker blocker(m_layerMask);
        const int index = m_layerMask->findData(static_cast<int>(layer->transform.mask));
        m_layerMask->setCurrentIndex(index >= 0 ? index : 0);
    }
    m_updatingInspector = false;
}

void MainWindow::updateCanvasHint() {
    if (!m_stageHint || !m_sceneCanvas) return;
    const QSize size = m_sceneCanvas->canvasSize();
    if (!size.isValid()) return;
    m_stageHint->setText(
        QStringLiteral("Canvas %1 × %2 · %3% · Drag to move · edge/corner handles resize · "
                       "top dot rotates · Alt+drag crops · Ctrl+wheel zooms · Ctrl+0 fits · "
                       "right-click for actions · drag dock separators to resize")
            .arg(size.width())
            .arg(size.height())
            .arg(m_sceneCanvas->zoomPercent()));
    if (m_zoomLabel) m_zoomLabel->setText(QStringLiteral("%1%").arg(m_sceneCanvas->zoomPercent()));
}

void MainWindow::showCanvasContextMenu(const QString &layerId, const QPoint &globalPosition) {
    if (!layerId.isEmpty()) {
        showLayerContextMenu(layerId, globalPosition);
        return;
    }
    if (!m_sceneCanvas) return;

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("canvasContextMenu"));
    QAction *selectAll = menu.addAction(QStringLiteral("Select all layers"));
    menu.addSeparator();
    QAction *addCamera = menu.addAction(StudioVisuals::icon(QStringLiteral("camera")),
                                        QStringLiteral("Add camera"));
    QAction *addImage = menu.addAction(StudioVisuals::icon(QStringLiteral("image")),
                                       QStringLiteral("Add image"));
    QAction *addText = menu.addAction(StudioVisuals::icon(QStringLiteral("text")),
                                      QStringLiteral("Add text"));
    QAction *addColor = menu.addAction(StudioVisuals::icon(QStringLiteral("color")),
                                       QStringLiteral("Add color"));
    menu.addSeparator();
    QAction *fitCanvas = menu.addAction(QStringLiteral("Fit canvas to view"));
    QAction *zoomOut = menu.addAction(QStringLiteral("Zoom out"));
    QAction *zoomIn = menu.addAction(QStringLiteral("Zoom in"));
    const bool editable = !hasActiveRecordingWork();
    for (QAction *action : {selectAll, addCamera, addImage, addText, addColor})
        action->setEnabled(editable);

    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) return;
    if (chosen == selectAll) {
        m_layerList->selectAll();
        m_sceneCanvas->clearLayerSelection();
        for (QListWidgetItem *item : m_layerList->selectedItems())
            m_sceneCanvas->selectLayer(item->data(Qt::UserRole).toString(), true);
    } else if (chosen == addCamera) {
        addStudioSource(1);
    } else if (chosen == addImage) {
        addStudioSource(2);
    } else if (chosen == addText) {
        addStudioSource(3);
    } else if (chosen == addColor) {
        addStudioSource(4);
    } else if (chosen == fitCanvas) {
        m_sceneCanvas->fitCanvas();
    } else if (chosen == zoomOut) {
        m_sceneCanvas->zoomOut();
    } else if (chosen == zoomIn) {
        m_sceneCanvas->zoomIn();
    }
}

void MainWindow::showLayerContextMenu(const QString &layerId, const QPoint &globalPosition) {
    if (!m_sceneDocument || !m_sceneCanvas) return;
    const SceneFormat format = static_cast<SceneFormat>(m_sceneFormat);
    const SceneLayer *layer = m_sceneDocument->layer(format, layerId);
    if (!layer) return;

    if (m_layerList) {
        QSignalBlocker blocker(m_layerList);
        for (int row = 0; row < m_layerList->count(); ++row)
            m_layerList->item(row)->setSelected(m_layerList->item(row)->data(Qt::UserRole).toString() == layerId);
    }
    m_sceneCanvas->clearLayerSelection();
    m_sceneCanvas->selectLayer(layerId);
    refreshLayerInspector();

    QMenu menu(this);
    menu.setObjectName(QStringLiteral("layerContextMenu"));
    QAction *duplicate = menu.addAction(QStringLiteral("Duplicate layer"));
    QAction *bringFront = menu.addAction(QStringLiteral("Bring to front"));
    QAction *sendBack = menu.addAction(QStringLiteral("Send to back"));
    menu.addSeparator();
    QAction *fitLayer = menu.addAction(QStringLiteral("Fit layer to canvas"));
    QAction *centerLayer = menu.addAction(QStringLiteral("Center layer"));
    QAction *rotateClockwise = menu.addAction(QStringLiteral("Rotate clockwise 90°"));
    QAction *rotateCounterClockwise = menu.addAction(QStringLiteral("Rotate counter-clockwise 90°"));
    QAction *resetTransform = menu.addAction(QStringLiteral("Reset transform"));
    menu.addSeparator();
    QAction *visibility = menu.addAction(layer->visible ? QStringLiteral("Hide layer")
                                                        : QStringLiteral("Show layer"));
    QAction *lock = menu.addAction(layer->locked ? QStringLiteral("Unlock layer")
                                                 : QStringLiteral("Lock layer"));
    menu.addSeparator();
    QAction *remove = menu.addAction(QStringLiteral("Delete layer"));

    const bool editable = !hasActiveRecordingWork();
    const bool transformable = editable && !layer->locked;
    for (QAction *action : {duplicate, bringFront, sendBack, fitLayer, centerLayer,
                            rotateClockwise, rotateCounterClockwise, resetTransform,
                            visibility, lock, remove})
        action->setEnabled(editable);
    for (QAction *action : {fitLayer, centerLayer, rotateClockwise,
                            rotateCounterClockwise, resetTransform, remove})
        action->setEnabled(transformable);

    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) return;
    if (chosen == duplicate) {
        const QString copyId = m_sceneDocument->duplicateLayer(format, layerId);
        if (copyId.isEmpty()) return;
        m_sceneCanvas->setDocument(m_sceneDocument.get(), format);
        refreshLayerList();
        m_sceneCanvas->selectLayer(copyId);
        saveCurrentProject();
    } else if (chosen == bringFront || chosen == sendBack) {
        const auto &layers = m_sceneDocument->composition(format).layers;
        int index = -1;
        for (int i = 0; i < layers.size(); ++i)
            if (layers.at(i).id == layerId) { index = i; break; }
        if (index < 0) return;
        const int destination = chosen == bringFront ? layers.size() - 1 : 0;
        m_sceneDocument->moveLayer(format, layerId, destination);
        m_sceneCanvas->setDocument(m_sceneDocument.get(), format);
        refreshLayerList();
        m_sceneCanvas->selectLayer(layerId);
        saveCurrentProject();
    } else if (chosen == fitLayer) {
        m_sceneCanvas->fitSelection();
    } else if (chosen == centerLayer) {
        m_sceneCanvas->centerSelection();
    } else if (chosen == rotateClockwise) {
        m_sceneCanvas->rotateSelection(90.0);
    } else if (chosen == rotateCounterClockwise) {
        m_sceneCanvas->rotateSelection(-90.0);
    } else if (chosen == resetTransform) {
        m_sceneCanvas->resetSelection();
    } else if (chosen == visibility) {
        m_sceneDocument->setLayerVisible(format, layerId, !layer->visible);
        m_sceneCanvas->refreshFromDocument();
        refreshLayerList();
        m_sceneCanvas->selectLayer(layerId);
        saveCurrentProject();
    } else if (chosen == lock) {
        m_sceneDocument->setLayerLocked(format, layerId, !layer->locked);
        m_sceneCanvas->refreshFromDocument();
        refreshLayerList();
        m_sceneCanvas->selectLayer(layerId);
        saveCurrentProject();
    } else if (chosen == remove) {
        deleteSelectedLayers();
    }
}

void MainWindow::showLayerListContextMenu(const QPoint &position) {
    if (!m_layerList) return;
    QListWidgetItem *item = m_layerList->itemAt(position);
    const QPoint globalPosition = m_layerList->viewport()->mapToGlobal(position);
    if (item) {
        showLayerContextMenu(item->data(Qt::UserRole).toString(), globalPosition);
    } else {
        showCanvasContextMenu({}, globalPosition);
    }
}

void MainWindow::showSourceListContextMenu(const QPoint &position) {
    if (!m_sourceList || !m_sceneDocument) return;
    QListWidgetItem *item = m_sourceList->itemAt(position);
    const QPoint globalPosition = m_sourceList->viewport()->mapToGlobal(position);
    if (!item) {
        showCanvasContextMenu({}, globalPosition);
        return;
    }
    m_sourceList->setCurrentItem(item);
    const QString sourceId = item->data(Qt::UserRole).toString();
    const SceneSource *source = m_sceneDocument->source(sourceId);
    if (!source) return;
    const SceneFormat format = static_cast<SceneFormat>(m_sceneFormat);
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("sourceContextMenu"));
    QAction *selectLayers = menu.addAction(QStringLiteral("Select layers using this source"));
    QAction *addLayer = menu.addAction(QStringLiteral("Add another layer"));
    QAction *removeSource = menu.addAction(QStringLiteral("Remove source"));
    const bool editable = !hasActiveRecordingWork();
    addLayer->setEnabled(editable);
    removeSource->setEnabled(editable && source->type != SceneSourceType::AirPlay);
    QAction *chosen = menu.exec(globalPosition);
    if (!chosen) return;
    if (chosen == selectLayers) {
        // Setting the current source above already selected its existing layers.
    } else if (chosen == addLayer) {
        const QString layerId = m_sceneDocument->addLayer(format, sourceId);
        if (layerId.isEmpty()) return;
        m_sceneCanvas->setDocument(m_sceneDocument.get(), format);
        refreshLayerList();
        m_sceneCanvas->selectLayer(layerId);
        saveCurrentProject();
    } else if (chosen == removeSource) {
        if (source->type == SceneSourceType::Camera && m_recordCamera)
            m_recordCamera->setChecked(false);
        if (!m_sceneDocument->removeSource(sourceId)) return;
        m_sceneCanvas->setDocument(m_sceneDocument.get(), format);
        refreshLayerList();
        saveCurrentProject();
    }
}

void MainWindow::showCameraMonitorContextMenu(const QPoint &globalPosition) {
    if (!m_cameraSelfView) return;
    QMenu menu(this);
    menu.setObjectName(QStringLiteral("cameraMonitorContextMenu"));
    QAction *editLayout = menu.addAction(QStringLiteral("Edit camera layout"));
    QAction *hideMonitor = menu.addAction(QStringLiteral("Hide live camera monitor"));
    QAction *chosen = menu.exec(globalPosition);
    if (chosen == editLayout) {
        if (m_editModeButton) m_editModeButton->click();
    } else if (chosen == hideMonitor) {
        m_cameraSelfView->setOverlayVisible(false);
    }
}

void MainWindow::refreshProjectList() {
    if (!m_projectList || !m_projectStore) return;
    QSignalBlocker blocker(m_projectList);
    m_projectList->clear();
    if (m_recoverProjectButton) m_recoverProjectButton->setEnabled(false);
    const auto projects = m_projectStore->projects();
    for (const ProjectSummary &project : projects) {
        const QString state = projectStateKey(project.state).toUpper();
        const QString title = project.title.isEmpty() ? QStringLiteral("Untitled recording") : project.title;
        auto *item = new QListWidgetItem(QIcon(StudioVisuals::projectThumbnail(
            title, project.state == ProjectState::Recoverable)),
            QStringLiteral("%1\n%2  ·  %3")
            .arg(title,
                  project.updatedAtUtc.toLocalTime().toString(QStringLiteral("dd MMM yyyy  HH:mm")), state),
            m_projectList);
        item->setData(Qt::UserRole, project.directory);
        item->setSizeHint(QSize(278, 214));
        if (project.state == ProjectState::Recoverable) item->setForeground(QColor(QStringLiteral("#f6c85f")));
    }
}

void MainWindow::setCameraPreviewEnabled(bool enabled) {
    if (!m_cameraPreviewEngine || !m_cameraSelfView) return;
    const bool recording = m_recordingSession &&
        (m_recordingSession->state() == RecordingState::Starting ||
         m_recordingSession->state() == RecordingState::Recording ||
         m_recordingSession->state() == RecordingState::Finalizing);
    if (!enabled) {
        if (!recording) m_cameraPreviewEngine->stop();
        m_cameraSelfView->setActive(false);
        return;
    }
    m_cameraSelfView->setActive(true);
    if (recording || m_cameraPreviewEngine->isRunning()) return;
    QString error;
    if (!m_cameraPreviewEngine->start(&error)) {
        m_cameraSelfView->setActive(false);
        if (m_recordingStatus) m_recordingStatus->setText(QStringLiteral("Camera preview unavailable · %1").arg(error));
        appendActivity(QStringLiteral("Camera"), error);
    }
}

void MainWindow::handleCameraPreviewFrame(const QImage &frame) {
    if (frame.isNull() || !m_sceneDocument) return;
    if (m_cameraSelfView) m_cameraSelfView->setFrame(frame);
    if (!m_sceneCanvas) return;
    for (const SceneSource &source : m_sceneDocument->sources()) {
        if (source.type == SceneSourceType::Camera)
            m_sceneCanvas->setSourcePreview(source.id, frame);
    }
}

bool MainWindow::openProjectDirectory(const QString &directory) {
    auto loaded = m_projectStore->load(directory);
    if (!loaded.ok()) {
        if (m_projectFeedback) m_projectFeedback->setText(loaded.error);
        return false;
    }
    m_sceneDocument = std::move(loaded.document);
    m_currentProject = std::make_unique<ProjectInfo>(loaded.project);
    m_sceneCanvas->setDocument(m_sceneDocument.get(), static_cast<SceneFormat>(m_sceneFormat));
    refreshLayerList();
    loadRecordedPreviews(*m_currentProject);
    if (m_projectFeedback) m_projectFeedback->clear();
    selectPage(0);
    setStudioMode(true);
    return true;
}

void MainWindow::toggleRecording() {
    const RecordingState current = m_recordingSession->state();
    if (current == RecordingState::Recording) {
        if (!m_recordingSession->stop() && m_recordingStatus)
            m_recordingStatus->setText(m_recordingSession->statusSummary());
        return;
    }
    if (current == RecordingState::Starting || current == RecordingState::Finalizing) {
        if (m_recordingStatus)
            m_recordingStatus->setText(m_recordingSession->statusSummary());
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
    RecordingOptions options;
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
    const ProjectLoadResult persisted = m_projectStore->load(m_currentProject->directory);
    if (!persisted.ok()) {
        m_recordingStatus->setText(QStringLiteral("Could not verify the project before export: %1")
                                       .arg(persisted.error));
        return;
    }
    if (persisted.project.state != ProjectState::Ready) {
        m_recordingStatus->setText(persisted.project.state == ProjectState::Recoverable
            ? QStringLiteral("Recover this project before export")
            : QStringLiteral("The project must be Ready before export"));
        return;
    }
    *m_currentProject = persisted.project;
    if (!saveCurrentProject()) {
        if (m_recordingStatus && m_recordingStatus->text().isEmpty())
            m_recordingStatus->setText(QStringLiteral("Could not save the project before export"));
        return;
    }
    QDir().mkpath(m_currentProject->exportsDirectory());
    const QString format = m_sceneFormat == static_cast<int>(SceneFormat::Vertical)
        ? QStringLiteral("vertical") : QStringLiteral("wide");
    const QString output = QDir(m_currentProject->exportsDirectory()).filePath(
        QStringLiteral("%1-%2.mp4").arg(format,
            QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    m_exportJob->start(*m_currentProject, *m_sceneDocument,
                       static_cast<SceneFormat>(m_sceneFormat), output);
}

bool MainWindow::saveCurrentProject() {
    if (!m_currentProject || !m_sceneDocument) return false;
    const QString error = m_projectStore->save(*m_currentProject, *m_sceneDocument);
    if (error.isEmpty()) return true;
    if (m_recordingStatus) m_recordingStatus->setText(error);
    appendActivity(QStringLiteral("Projects"),
                   QStringLiteral("Could not save the current project: %1").arg(error));
    return false;
}

void MainWindow::loadRecordedPreviews(const ProjectInfo &project) {
    const QStringList airplay = QDir(project.airplayDirectory()).entryList(
        {QStringLiteral("video-*.mkv")}, QDir::Files, QDir::Name);
    const QStringList cameras = QDir(project.presenterDirectory()).entryList(
        {QStringLiteral("camera-*.mkv")}, QDir::Files, QDir::Name);
    const QString airplayPath = airplay.isEmpty() ? QString() :
        QDir(project.airplayDirectory()).filePath(airplay.first());
    const QString cameraPath = cameras.isEmpty() ? QString() :
        QDir(project.presenterDirectory()).filePath(cameras.first());
    if (airplayPath.isEmpty() && cameraPath.isEmpty()) return;
    const QString projectId = project.id;
    QThread *thread = QThread::create([this, projectId, airplayPath, cameraPath]() {
        const VideoPreviewResult airplayPreview = loadVideoPreview(
            airplayPath, QStringLiteral("AirPlay"));
        const VideoPreviewResult cameraPreview = loadVideoPreview(
            cameraPath, QStringLiteral("camera"));
        QMetaObject::invokeMethod(this, [this, projectId, airplayPreview, cameraPreview]() {
            if (!m_currentProject || m_currentProject->id != projectId || !m_sceneCanvas || !m_sceneDocument)
                return;
            for (const SceneSource &source : m_sceneDocument->sources()) {
                if (source.type == SceneSourceType::AirPlay && !airplayPreview.image.isNull())
                    m_sceneCanvas->setSourcePreview(source.id, airplayPreview.image);
                if (source.type == SceneSourceType::Camera && !cameraPreview.image.isNull())
                    m_sceneCanvas->setSourcePreview(source.id, cameraPreview.image);
            }
            QStringList previewErrors;
            if (!airplayPreview.error.isEmpty()) previewErrors.append(airplayPreview.error);
            if (!cameraPreview.error.isEmpty()) previewErrors.append(cameraPreview.error);
            for (const QString &error : previewErrors) {
                appendActivity(QStringLiteral("Preview"), error);
            }
            if (!previewErrors.isEmpty() && m_recordingStatus &&
                (!m_recordingSession ||
                 m_recordingSession->state() == RecordingState::Idle ||
                 m_recordingSession->state() == RecordingState::Failed)) {
                m_recordingStatus->setText(
                    QStringLiteral("Recorded preview unavailable for one or more tracks"));
            }
        }, Qt::QueuedConnection);
    });
    m_previewThreads.append(thread);
    connect(thread, &QThread::finished, this, [this, thread]() { m_previewThreads.removeAll(thread); });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}


QWidget *MainWindow::createPlayerPage() {
    m_playerPage = new QWidget(this);
    m_playerPage->setObjectName(QStringLiteral("page"));
    m_playerPageLayout = new QHBoxLayout(m_playerPage);
    m_playerPageLayout->setObjectName(QStringLiteral("playerPageLayout"));
    m_playerPageLayout->setContentsMargins(0, 0, 0, 0);
    m_playerPageLayout->setSpacing(0);

    m_playerCard = card(m_playerPage);
    m_playerCard->setObjectName(QStringLiteral("playerCard"));
    m_playerLayout = new QVBoxLayout(m_playerCard);
    m_playerLayout->setObjectName(QStringLiteral("playerLayout"));
    m_playerLayout->setContentsMargins(0, 0, 0, 0);
    m_playerLayout->setSpacing(0);

    m_playerChrome = new QWidget(m_playerCard);
    m_playerChrome->setObjectName(QStringLiteral("playerChrome"));
    m_playerChrome->setFixedHeight(56);
    auto *toolbar = new QHBoxLayout(m_playerChrome);
    toolbar->setContentsMargins(18, 9, 18, 9);
    toolbar->setSpacing(5);
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
    toolbar->addSpacing(8);
    auto *formatLabel = new QLabel(QStringLiteral("OUTPUT"), m_playerChrome);
    formatLabel->setObjectName(QStringLiteral("pageEyebrow"));
    toolbar->addWidget(formatLabel);
    toolbar->addStretch();
    auto *embedded = new QLabel(QStringLiteral("D3D11 · EMBEDDED"), m_playerChrome);
    embedded->setObjectName(QStringLiteral("miniBadge"));
    toolbar->addWidget(embedded);
    m_playerLayout->addWidget(m_playerChrome);

    m_previewStack = new QStackedWidget(m_playerCard);
    m_previewStack->setObjectName(QStringLiteral("previewStack"));
    m_previewStack->installEventFilter(this);
    m_videoSurface = new VideoSurface(m_previewStack);
    m_sceneCanvas = new SceneCanvas(m_previewStack);
    m_sceneCanvas->setDocument(m_sceneDocument.get(), SceneFormat::Wide);
    m_previewStack->addWidget(m_videoSurface);
    m_previewStack->addWidget(m_sceneCanvas);
    connect(m_sceneCanvas, &SceneCanvas::zoomChanged, this,
            [this](int) { updateCanvasHint(); });
    m_stageFrame = new QWidget(m_playerCard);
    m_stageFrame->setObjectName(QStringLiteral("stageFrame"));
    m_stageFrame->installEventFilter(this);
    m_playerCard->installEventFilter(this);
    m_stageLayout = new QVBoxLayout(m_stageFrame);
    m_stageLayout->setObjectName(QStringLiteral("stageLayout"));
    m_stageLayout->setContentsMargins(20, 18, 20, 12);
    m_stageLayout->setSpacing(8);
    m_stageLayout->addWidget(m_previewStack, 1);
    m_stageFooter = new QWidget(m_stageFrame);
    m_stageFooter->setObjectName(QStringLiteral("stageFooter"));
    auto *stageFooterLayout = new QHBoxLayout(m_stageFooter);
    stageFooterLayout->setContentsMargins(0, 0, 0, 0);
    stageFooterLayout->setSpacing(8);
    m_stageHint = new QLabel(m_stageFooter);
    m_stageHint->setObjectName(QStringLiteral("stageHint"));
    m_stageHint->setAlignment(Qt::AlignCenter);
    m_stageHint->setWordWrap(true);
    m_stageHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    stageFooterLayout->addWidget(m_stageHint, 1);
    m_stageLayout->addWidget(m_stageFooter);
    m_playerLayout->addWidget(m_stageFrame, 1);
    m_cameraSelfView = new CameraSelfView(m_previewStack);
    connect(m_sceneCanvas, &SceneCanvas::contextMenuRequested, this,
            &MainWindow::showCanvasContextMenu);
    connect(m_cameraSelfView, &CameraSelfView::contextMenuRequested, this,
            &MainWindow::showCameraMonitorContextMenu);

    m_playerControls = new QWidget(m_playerCard);
    m_playerControls->setObjectName(QStringLiteral("playerControls"));
    auto *controls = new QHBoxLayout(m_playerControls);
    controls->setContentsMargins(12, 7, 12, 9);
    controls->setSpacing(5);
    const QList<QPair<QString, std::function<void()>>> commands {
        {QStringLiteral("Undo"), [this]() { m_sceneCanvas->undoStack()->undo(); }},
        {QStringLiteral("Redo"), [this]() { m_sceneCanvas->undoStack()->redo(); }},
        {QStringLiteral("Fit"), [this]() { m_sceneCanvas->fitSelection(); }},
        {QStringLiteral("Center"), [this]() { m_sceneCanvas->centerSelection(); }},
        {QStringLiteral("Reset"), [this]() { m_sceneCanvas->resetSelection(); }}
    };
    int commandIndex = 0;
    for (const auto &command : commands) {
        const QString compactText = commandIndex == 0 ? QStringLiteral("↶")
            : commandIndex == 1 ? QStringLiteral("↷") : command.first;
        auto *button = new QPushButton(compactText, m_playerControls);
        button->setObjectName(QStringLiteral("dockButton"));
        button->setToolTip(command.first);
        // Keep the edit toolbar usable beside the zoom controls at the
        // smallest supported window size.  The full action names remain in
        // tooltips, while the compact buttons preserve the stage width.
        button->setFixedWidth(commandIndex < 2 ? 34 : 58);
        button->setVisible(false);
        connect(button, &QPushButton::clicked, this, command.second);
        controls->addWidget(button);
        m_editActionButtons.append(button);
        ++commandIndex;
    }
    m_canvasViewControls = new QWidget(m_stageFooter);
    m_canvasViewControls->setObjectName(QStringLiteral("canvasViewControls"));
    auto *viewControls = new QHBoxLayout(m_canvasViewControls);
    viewControls->setContentsMargins(4, 0, 4, 0);
    viewControls->setSpacing(3);
    auto *viewLabel = new QLabel(QStringLiteral("VIEW"), m_canvasViewControls);
    viewLabel->setObjectName(QStringLiteral("detailKey"));
    viewControls->addWidget(viewLabel);
    m_zoomOutButton = new QPushButton(QStringLiteral("−"), m_canvasViewControls);
    m_zoomOutButton->setObjectName(QStringLiteral("zoomOutButton"));
    m_zoomOutButton->setAccessibleName(QStringLiteral("zoomOutButton"));
    m_zoomOutButton->setToolTip(QStringLiteral("Zoom out (Ctrl−wheel / Ctrl+-)"));
    m_zoomOutButton->setFixedWidth(30);
    m_zoomLabel = new QLabel(QStringLiteral("100%"), m_canvasViewControls);
    m_zoomLabel->setObjectName(QStringLiteral("zoomLabel"));
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    m_zoomLabel->setMinimumWidth(38);
    m_zoomInButton = new QPushButton(QStringLiteral("+"), m_canvasViewControls);
    m_zoomInButton->setObjectName(QStringLiteral("zoomInButton"));
    m_zoomInButton->setAccessibleName(QStringLiteral("zoomInButton"));
    m_zoomInButton->setToolTip(QStringLiteral("Zoom in (Ctrl−wheel / Ctrl++)"));
    m_zoomInButton->setFixedWidth(30);
    m_fitCanvasButton = new QPushButton(QStringLiteral("Fit"), m_canvasViewControls);
    m_fitCanvasButton->setObjectName(QStringLiteral("fitCanvasButton"));
    m_fitCanvasButton->setAccessibleName(QStringLiteral("fitCanvasButton"));
    m_fitCanvasButton->setToolTip(QStringLiteral("Fit the complete canvas to the workspace (Ctrl+0)"));
    viewControls->addWidget(m_zoomOutButton);
    viewControls->addWidget(m_zoomLabel);
    viewControls->addWidget(m_zoomInButton);
    viewControls->addWidget(m_fitCanvasButton);
    connect(m_zoomOutButton, &QPushButton::clicked, m_sceneCanvas, &SceneCanvas::zoomOut);
    connect(m_zoomInButton, &QPushButton::clicked, m_sceneCanvas, &SceneCanvas::zoomIn);
    connect(m_fitCanvasButton, &QPushButton::clicked, m_sceneCanvas, &SceneCanvas::fitCanvas);
    auto *zoomInAction = new QAction(QStringLiteral("Zoom in canvas"), this);
    zoomInAction->setShortcut(QKeySequence::ZoomIn);
    zoomInAction->setShortcutContext(Qt::WindowShortcut);
    connect(zoomInAction, &QAction::triggered, m_sceneCanvas, &SceneCanvas::zoomIn);
    addAction(zoomInAction);
    auto *zoomOutAction = new QAction(QStringLiteral("Zoom out canvas"), this);
    zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    zoomOutAction->setShortcutContext(Qt::WindowShortcut);
    connect(zoomOutAction, &QAction::triggered, m_sceneCanvas, &SceneCanvas::zoomOut);
    addAction(zoomOutAction);
    auto *fitCanvasAction = new QAction(QStringLiteral("Fit canvas to view"), this);
    fitCanvasAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    fitCanvasAction->setShortcutContext(Qt::WindowShortcut);
    connect(fitCanvasAction, &QAction::triggered, m_sceneCanvas, &SceneCanvas::fitCanvas);
    addAction(fitCanvasAction);
    m_canvasViewControls->hide();
    stageFooterLayout->addWidget(m_canvasViewControls);
    controls->addStretch();
    m_fullscreenButton = new QPushButton(m_playerControls);
    m_fullscreenButton->setObjectName(QStringLiteral("fullscreenButton"));
    m_fullscreenButton->setIcon(StudioVisuals::icon(QStringLiteral("fullscreen")));
    m_fullscreenButton->setIconSize(QSize(19, 19));
    m_fullscreenButton->setFixedWidth(42);
    m_fullscreenButton->setToolTip(QStringLiteral("Fullscreen (F11)"));
    connect(m_fullscreenButton, &QPushButton::clicked, this, &MainWindow::enterFullscreen);
    controls->addWidget(m_fullscreenButton);
    m_playerLayout->addWidget(m_playerControls);
    updateCanvasHint();

    m_sessionPanel = card(m_playerPage);
    m_sessionPanel->setObjectName(QStringLiteral("studioDock"));
    m_sessionPanel->setMinimumWidth(310);
    m_sessionPanel->setMaximumWidth(600);
    auto *dock = new QVBoxLayout(m_sessionPanel);
    dock->setContentsMargins(16, 16, 16, 14);
    dock->setSpacing(8);
    auto *dockHeader = new QHBoxLayout;
    auto *dockHeading = new QLabel(QStringLiteral("Studio controls"), m_sessionPanel);
    dockHeading->setObjectName(QStringLiteral("dockHeading"));
    dockHeader->addWidget(dockHeading);
    dockHeader->addStretch();
    m_sourceCount = mutedLabel(QStringLiteral("1 source"), m_sessionPanel);
    dockHeader->addWidget(m_sourceCount);
    dock->addLayout(dockHeader);
    auto *sourceAreaSplitter = new QSplitter(Qt::Vertical, m_sessionPanel);
    sourceAreaSplitter->setObjectName(QStringLiteral("sourceAreaSplitter"));
    sourceAreaSplitter->setAccessibleName(QStringLiteral("sourcesVerticalSplitter"));
    sourceAreaSplitter->setHandleWidth(7);
    sourceAreaSplitter->setChildrenCollapsible(false);
    m_sourceList = new QListWidget(sourceAreaSplitter);
    m_sourceList->setObjectName(QStringLiteral("sourceList"));
    m_sourceList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_sourceList->setMinimumHeight(72);
    sourceAreaSplitter->addWidget(m_sourceList);
    auto *belowSources = new QWidget(sourceAreaSplitter);
    auto *belowSourcesLayout = new QVBoxLayout(belowSources);
    belowSourcesLayout->setContentsMargins(0, 0, 0, 0);
    belowSourcesLayout->setSpacing(8);
    auto *sourceRow = new QGridLayout;
    sourceRow->setSpacing(6);
    const QList<QPair<QString, int>> additions{{QStringLiteral("Camera"), 1}, {QStringLiteral("Image"), 2},
                                               {QStringLiteral("Text"), 3}, {QStringLiteral("Color"), 4}};
    int sourceIndex = 0;
    for (const auto &entry : additions) {
        auto *button = new QPushButton(QStringLiteral("+") + entry.first, belowSources);
        button->setObjectName(QStringLiteral("sourceButton"));
        const QString icon = entry.second == 1 ? QStringLiteral("camera")
            : entry.second == 2 ? QStringLiteral("image")
            : entry.second == 3 ? QStringLiteral("text") : QStringLiteral("color");
        button->setIcon(StudioVisuals::icon(icon));
        button->setIconSize(QSize(17, 17));
        connect(button, &QPushButton::clicked, this, [this, entry]() { addStudioSource(entry.second); });
        sourceRow->addWidget(button, sourceIndex / 2, sourceIndex % 2);
        ++sourceIndex;
    }
    belowSourcesLayout->addLayout(sourceRow);
    auto *layersTitle = new QLabel(QStringLiteral("LAYERS"), belowSources);
    layersTitle->setObjectName(QStringLiteral("dockTitle"));
    belowSourcesLayout->addWidget(layersTitle);

    auto *layerAreaSplitter = new QSplitter(Qt::Vertical, belowSources);
    layerAreaSplitter->setObjectName(QStringLiteral("layerAreaSplitter"));
    layerAreaSplitter->setAccessibleName(QStringLiteral("layersVerticalSplitter"));
    layerAreaSplitter->setHandleWidth(7);
    layerAreaSplitter->setChildrenCollapsible(false);
    m_layerList = new QListWidget(layerAreaSplitter);
    m_layerList->setObjectName(QStringLiteral("layerList"));
    m_layerList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_layerList->setMinimumHeight(72);
    m_layerList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_layerList->setDragDropMode(QAbstractItemView::InternalMove);
    layerAreaSplitter->addWidget(m_layerList);
    auto *lowerScroll = new InspectorScrollArea(layerAreaSplitter);
    lowerScroll->setObjectName(QStringLiteral("layerInspectorScroll"));
    lowerScroll->setFrameShape(QFrame::NoFrame);
    lowerScroll->setWidgetResizable(true);
    lowerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    lowerScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    lowerScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    lowerScroll->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    lowerScroll->setMinimumSize(0, 0);
    lowerScroll->setMinimumHeight(0);
    auto *lowerPanel = new QWidget;
    lowerPanel->setObjectName(QStringLiteral("layerInspectorContent"));
    lowerPanel->setMinimumSize(0, 0);
    lowerScroll->setWidget(lowerPanel);
    auto *lowerLayout = new QVBoxLayout(lowerPanel);
    lowerLayout->setContentsMargins(0, 0, 0, 0);
    lowerLayout->setSpacing(8);
    auto *layerRow = new QHBoxLayout;
    auto *up = new QPushButton(QStringLiteral("Up"), lowerPanel);
    auto *down = new QPushButton(QStringLiteral("Down"), lowerPanel);
    auto *lock = new QPushButton(QStringLiteral("Lock"), lowerPanel);
    auto *remove = new QPushButton(QStringLiteral("Remove"), lowerPanel);
    up->setIcon(StudioVisuals::icon(QStringLiteral("up")));
    down->setIcon(StudioVisuals::icon(QStringLiteral("down")));
    lock->setIcon(StudioVisuals::icon(QStringLiteral("lock")));
    remove->setIcon(StudioVisuals::icon(QStringLiteral("trash")));
    for (auto *button : {up, down, lock, remove}) {
        button->setText(QString());
        button->setObjectName(QStringLiteral("iconButton"));
        button->setIconSize(QSize(17, 17));
        layerRow->addWidget(button);
    }
    up->setToolTip(QStringLiteral("Move layer forward"));
    down->setToolTip(QStringLiteral("Move layer backward"));
    lock->setToolTip(QStringLiteral("Lock or unlock layer"));
    remove->setToolTip(QStringLiteral("Remove layer"));
    remove->setAccessibleName(QStringLiteral("deleteLayerButton"));
    layerRow->addStretch();
    lowerLayout->addLayout(layerRow);
    m_transformPanel = new QWidget(lowerPanel);
    m_transformPanel->setObjectName(QStringLiteral("transformPanel"));
    auto *transformLayout = new QVBoxLayout(m_transformPanel);
    transformLayout->setContentsMargins(0, 2, 0, 0);
    transformLayout->setSpacing(7);
    auto *transformTitle = new QLabel(QStringLiteral("TRANSFORM"), m_transformPanel);
    transformTitle->setObjectName(QStringLiteral("dockTitle"));
    transformLayout->addWidget(transformTitle);
    auto *geometry = new QGridLayout;
    geometry->setHorizontalSpacing(6);
    geometry->setVerticalSpacing(6);
    geometry->setColumnStretch(1, 1);
    geometry->setColumnStretch(3, 1);
    auto makeField = [this, geometry](const QString &label, int row, int column,
                                      qreal minimum, qreal maximum,
                                      QDoubleSpinBox *&field) {
        auto *caption = new QLabel(label, m_transformPanel);
        caption->setObjectName(QStringLiteral("detailKey"));
        geometry->addWidget(caption, row, column * 2);
        field = new QDoubleSpinBox(m_transformPanel);
        field->setObjectName(QStringLiteral("transformSpin"));
        field->setAccessibleName(QStringLiteral("layerTransform%1").arg(label));
        field->setRange(minimum, maximum);
        field->setDecimals(label == QStringLiteral("R") ? 1 : 0);
        field->setSingleStep(label == QStringLiteral("R") ? 1.0 : 4.0);
        geometry->addWidget(field, row, column * 2 + 1);
    };
    makeField(QStringLiteral("X"), 0, 0, -7680, 7680, m_layerX);
    makeField(QStringLiteral("Y"), 0, 1, -7680, 7680, m_layerY);
    makeField(QStringLiteral("W"), 1, 0, 1, 7680, m_layerWidth);
    makeField(QStringLiteral("H"), 1, 1, 1, 7680, m_layerHeight);
    makeField(QStringLiteral("R"), 2, 0, -180, 180, m_layerRotation);
    transformLayout->addLayout(geometry);
    auto *appearanceRow = new QHBoxLayout;
    m_layerOpacity = new QComboBox(m_transformPanel);
    m_layerOpacity->setObjectName(QStringLiteral("compactCombo"));
    m_layerOpacity->setAccessibleName(QStringLiteral("layerOpacity"));
    m_layerOpacity->addItem(QStringLiteral("Opacity 100%"), 1.0);
    m_layerOpacity->addItem(QStringLiteral("Opacity 75%"), .75);
    m_layerOpacity->addItem(QStringLiteral("Opacity 50%"), .5);
    m_layerMask = new QComboBox(m_transformPanel);
    m_layerMask->setObjectName(QStringLiteral("compactCombo"));
    m_layerMask->setAccessibleName(QStringLiteral("layerMask"));
    m_layerMask->addItem(QStringLiteral("No mask"), static_cast<int>(SceneMask::None));
    m_layerMask->addItem(QStringLiteral("Rounded"), static_cast<int>(SceneMask::RoundedRectangle));
    m_layerMask->addItem(QStringLiteral("Circle"), static_cast<int>(SceneMask::Circle));
    appearanceRow->addWidget(m_layerOpacity);
    appearanceRow->addWidget(m_layerMask);
    transformLayout->addLayout(appearanceRow);
    m_transformPanel->setVisible(false);
    lowerLayout->addWidget(m_transformPanel);

    auto applyGeometry = [this](double) {
        if (m_updatingInspector || !m_sceneCanvas || !m_layerX || !m_layerY ||
            !m_layerWidth || !m_layerHeight || !m_layerRotation) return;
        m_sceneCanvas->setSelectionGeometry(
            QRectF(m_layerX->value(), m_layerY->value(),
                   m_layerWidth->value(), m_layerHeight->value()),
            m_layerRotation->value());
    };
    for (QDoubleSpinBox *field : {m_layerX, m_layerY, m_layerWidth,
                                  m_layerHeight, m_layerRotation})
        connect(field, &QDoubleSpinBox::valueChanged, this, applyGeometry);

    auto *confidenceTitle = new QLabel(QStringLiteral("RECORDING CONFIDENCE"), lowerPanel);
    confidenceTitle->setObjectName(QStringLiteral("dockTitle"));
    lowerLayout->addWidget(confidenceTitle);
    m_recordCamera = new QCheckBox(QStringLiteral("Camera"), lowerPanel);
    m_recordCamera->setToolTip(QStringLiteral("Include the presenter camera as an independent track"));
    m_recordCamera->setObjectName(QStringLiteral("dockToggle"));
    m_recordMicrophone = new QCheckBox(QStringLiteral("Microphone"), lowerPanel);
    m_recordMicrophone->setToolTip(QStringLiteral("Include the microphone as an independent track"));
    m_recordMicrophone->setObjectName(QStringLiteral("dockToggle"));
    connect(m_recordCamera, &QCheckBox::toggled, this, [this](bool enabled) {
        setCameraPreviewEnabled(enabled);
    });
    m_recordingStatus = mutedLabel(QStringLiteral("Ready to record"), lowerPanel);
    lowerLayout->addWidget(m_recordingStatus);
    auto *recordRow = new QHBoxLayout;
    m_recordButton = new QPushButton(QStringLiteral("Start recording"), lowerPanel);
    m_recordButton->setObjectName(QStringLiteral("recordButton"));
    connect(m_recordButton, &QPushButton::clicked, this, &MainWindow::toggleRecording);
    auto *exportButton = new QPushButton(QStringLiteral("Export MP4"), lowerPanel);
    exportButton->setObjectName(QStringLiteral("secondaryButton"));
    exportButton->setIcon(StudioVisuals::icon(QStringLiteral("export")));
    exportButton->setAccessibleName(QStringLiteral("exportButton"));
    connect(exportButton, &QPushButton::clicked, this, &MainWindow::exportCurrentProject);
    recordRow->addWidget(exportButton, 1);
    lowerLayout->addLayout(recordRow);

    m_sessionState = new QLabel(QStringLiteral("Receiver stopped"), lowerPanel);
    m_sessionState->setObjectName(QStringLiteral("sessionMini"));
    m_deviceName = new QLabel(QStringLiteral("—"), lowerPanel);
    m_deviceModel = mutedLabel(QStringLiteral("Waiting for a connection"), lowerPanel);
    m_resolution = new QLabel(QStringLiteral("—"), lowerPanel);
    m_duration = new QLabel(QStringLiteral("00:00"), lowerPanel);
    m_duration->setObjectName(QStringLiteral("recordTime"));
    m_networkAddress = new QLabel(NetworkDiagnostics::primaryAddress(), lowerPanel);
    m_securitySummary = mutedLabel({}, lowerPanel);
    auto *streamRow = new QHBoxLayout;
    streamRow->addWidget(m_sessionState, 1);
    streamRow->addWidget(m_duration);
    lowerLayout->addLayout(streamRow);
    lowerLayout->addWidget(m_deviceName);
    lowerLayout->addWidget(m_resolution);
    lowerLayout->addWidget(m_networkAddress);
    lowerLayout->addWidget(m_deviceModel);
    lowerLayout->addWidget(m_securitySummary);
    layerAreaSplitter->addWidget(lowerScroll);
    layerAreaSplitter->setStretchFactor(0, 1);
    layerAreaSplitter->setStretchFactor(1, 0);
    layerAreaSplitter->setSizes({180, 420});
    belowSourcesLayout->addWidget(layerAreaSplitter, 1);
    sourceAreaSplitter->addWidget(belowSources);
    sourceAreaSplitter->setStretchFactor(0, 0);
    sourceAreaSplitter->setStretchFactor(1, 1);
    sourceAreaSplitter->setSizes({128, 620});
    dock->addWidget(sourceAreaSplitter, 1);

    // The parent layouts are not sized until the first event-loop turn.  Set
    // both initial splits after the complete dock is attached so a fresh
    // workspace starts balanced instead of collapsing either list to its
    // minimum while still leaving the inspector scrollable.
    const auto applyInitialDockSplits = [sourceAreaSplitter, layerAreaSplitter]() {
        const int total = sourceAreaSplitter->height();
        if (total <= 0) return;
        const QList<int> sourceSizes = sourceAreaSplitter->sizes();
        if (sourceSizes.size() == 2 && sourceSizes.at(0) > 260) {
            const int sourceHeight = qBound(112, total / 4, 240);
            sourceAreaSplitter->setSizes({sourceHeight, qMax(1, total - sourceHeight)});
        }
        const int nestedTotal = layerAreaSplitter->height();
        if (nestedTotal <= 0) return;
        const QList<int> layerSizes = layerAreaSplitter->sizes();
        if (layerSizes.size() == 2 && layerSizes.at(0) < 120) {
            const int listHeight = qBound(120, nestedTotal / 3, 260);
            layerAreaSplitter->setSizes({listHeight, qMax(1, nestedTotal - listHeight)});
        }
    };
    QTimer::singleShot(0, sourceAreaSplitter, applyInitialDockSplits);
    // A second pass is needed for the standalone launch path: the first pass
    // can run before the top-level window performs its final layout pass.
    QTimer::singleShot(100, sourceAreaSplitter, applyInitialDockSplits);
    controls->insertWidget(5, m_recordCamera);
    controls->insertWidget(6, m_recordMicrophone);
    controls->insertWidget(7, m_duration);
    controls->insertWidget(8, m_recordButton);
    m_playerSplitter = new QSplitter(Qt::Horizontal, m_playerPage);
    m_playerSplitter->setObjectName(QStringLiteral("playerSplitter"));
    m_playerSplitter->setHandleWidth(8);
    m_playerSplitter->setChildrenCollapsible(false);
    m_playerSplitter->setAccessibleName(QStringLiteral("studioWorkspaceSplitter"));
    m_playerSplitter->setStretchFactor(0, 1);
    m_playerSplitter->setStretchFactor(1, 0);
    m_playerSplitter->addWidget(m_playerCard);
    m_playerSplitter->addWidget(m_sessionPanel);
    m_playerSplitter->setSizes({900, 348});
    m_playerPageLayout->addWidget(m_playerSplitter);

    connect(m_layerList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_sceneCanvas->clearLayerSelection();
        for (QListWidgetItem *item : m_layerList->selectedItems())
            m_sceneCanvas->selectLayer(item->data(Qt::UserRole).toString(), true);
        refreshLayerInspector();
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
        refreshLayerInspector();
    });
    connect(m_layerList, &QListWidget::customContextMenuRequested, this,
            &MainWindow::showLayerListContextMenu);
    connect(m_sourceList, &QListWidget::customContextMenuRequested, this,
            &MainWindow::showSourceListContextMenu);
    m_layerList->installEventFilter(this);
    m_layerList->viewport()->installEventFilter(this);
    connect(m_sourceList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current, QListWidgetItem *) {
        if (!current || !m_layerList || !m_sceneDocument) return;
        const QString sourceId = current->data(Qt::UserRole).toString();
        m_layerList->clearSelection();
        for (int row = 0; row < m_layerList->count(); ++row) {
            const QString layerId = m_layerList->item(row)->data(Qt::UserRole).toString();
            const SceneLayer *layer = m_sceneDocument->layer(
                static_cast<SceneFormat>(m_sceneFormat), layerId);
            if (layer && layer->sourceId == sourceId) m_layerList->item(row)->setSelected(true);
        }
    });
    connect(m_sceneCanvas, &SceneCanvas::sceneChanged, this, [this]() {
        refreshLayerInspector();
        saveCurrentProject();
    });
    connect(m_sceneCanvas, &SceneCanvas::layersChanged, this, [this]() {
        refreshLayerList();
    });
    connect(m_layerOpacity, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_updatingInspector && m_layerOpacity)
            m_sceneCanvas->setSelectionOpacity(m_layerOpacity->currentData().toDouble());
    });
    connect(m_layerMask, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!m_updatingInspector && m_layerMask)
            m_sceneCanvas->setSelectionMask(static_cast<SceneMask>(m_layerMask->currentData().toInt()));
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
        if (auto *item = m_layerList->currentItem(); item &&
            m_sceneCanvas->selectedLayerIds().isEmpty()) {
            m_sceneCanvas->selectLayer(item->data(Qt::UserRole).toString());
        }
        deleteSelectedLayers();
    });
    m_deleteLayerAction = new QAction(QStringLiteral("Delete selected layer(s)"), this);
    m_deleteLayerAction->setObjectName(QStringLiteral("deleteLayerAction"));
    m_deleteLayerAction->setShortcuts({QKeySequence(Qt::Key_Delete),
                                       QKeySequence(Qt::Key_Backspace)});
    m_deleteLayerAction->setShortcutContext(Qt::WindowShortcut);
    m_deleteLayerAction->setToolTip(QStringLiteral("Delete selected layer(s) (Delete / Backspace)"));
    connect(m_deleteLayerAction, &QAction::triggered, this, [this]() { deleteSelectedLayers(); });
    addAction(m_deleteLayerAction);
    refreshLayerList();
    return m_playerPage;
}

QWidget *MainWindow::createProjectsPage() {
    auto *page = new QWidget(this);
    page->setObjectName(QStringLiteral("page"));
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(36, 28, 36, 30);
    layout->setSpacing(16);
    auto *headingRow = new QHBoxLayout;
    auto *headingCopy = new QVBoxLayout;
    auto *heading = new QLabel(QStringLiteral("Projects"), page);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    headingCopy->addWidget(heading);
    auto *intro = mutedLabel(QStringLiteral(
        "Recordings stay local and editable. Interrupted sessions are marked Recoverable; finalized segments are never discarded."), page);
    headingCopy->addWidget(intro);
    headingRow->addLayout(headingCopy, 1);
    auto *openFolder = new QPushButton(QStringLiteral("Open recordings folder"), page);
    openFolder->setObjectName(QStringLiteral("secondaryButton"));
    connect(openFolder, &QPushButton::clicked, this, [this]() {
        if (QDesktopServices::openUrl(QUrl::fromLocalFile(m_projectStore->rootDirectory())))
            return;
        if (m_projectFeedback) {
            m_projectFeedback->setText(
                QStringLiteral("Could not open the recordings folder in File Explorer"));
        }
        appendActivity(QStringLiteral("Projects"),
                       QStringLiteral("Could not open the recordings folder"));
    });
    headingRow->addWidget(openFolder, 0, Qt::AlignBottom);
    layout->addLayout(headingRow);
    auto *projectCard = card(page);
    projectCard->setObjectName(QStringLiteral("projectSurface"));
    auto *projectLayout = new QVBoxLayout(projectCard);
    projectLayout->setContentsMargins(18, 18, 18, 18);
    auto *row = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Local projects"), projectCard);
    title->setObjectName(QStringLiteral("cardTitle"));
    row->addWidget(title);
    row->addStretch();
    projectLayout->addLayout(row);
    m_projectList = new QListWidget(projectCard);
    m_projectList->setObjectName(QStringLiteral("projectList"));
    m_projectList->setViewMode(QListView::IconMode);
    m_projectList->setResizeMode(QListView::Adjust);
    m_projectList->setMovement(QListView::Static);
    m_projectList->setWrapping(true);
    m_projectList->setWordWrap(true);
    m_projectList->setSpacing(12);
    m_projectList->setIconSize(QSize(260, 146));
    m_projectList->setGridSize(QSize(286, 224));
    projectLayout->addWidget(m_projectList, 1);
    m_projectFeedback = mutedLabel(QString(), projectCard);
    m_projectFeedback->setObjectName(QStringLiteral("projectFeedback"));
    m_projectFeedback->setAccessibleName(QStringLiteral("projectFeedback"));
    m_projectFeedback->setWordWrap(true);
    projectLayout->addWidget(m_projectFeedback);
    auto *actions = new QHBoxLayout;
    auto *open = new QPushButton(QStringLiteral("Open in Studio"), projectCard);
    open->setObjectName(QStringLiteral("primaryButton"));
    open->setAccessibleName(QStringLiteral("openProjectButton"));
    connect(open, &QPushButton::clicked, this, [this]() {
        auto *item = m_projectList ? m_projectList->currentItem() : nullptr;
        if (!item) return;
        openProjectDirectory(item->data(Qt::UserRole).toString());
    });
    actions->addWidget(open);
    m_recoverProjectButton = new QPushButton(QStringLiteral("Recover session"), projectCard);
    m_recoverProjectButton->setObjectName(QStringLiteral("secondaryButton"));
    m_recoverProjectButton->setAccessibleName(QStringLiteral("recoverProjectButton"));
    m_recoverProjectButton->setEnabled(false);
    connect(m_projectList, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *current) {
        bool recoverable = false;
        if (current) {
            const auto loaded = m_projectStore->load(current->data(Qt::UserRole).toString());
            recoverable = loaded.ok() && loaded.project.state == ProjectState::Recoverable;
            if (!loaded.ok() && m_projectFeedback) m_projectFeedback->setText(loaded.error);
            else if (m_projectFeedback) m_projectFeedback->clear();
        }
        m_recoverProjectButton->setEnabled(recoverable);
    });
    connect(m_recoverProjectButton, &QPushButton::clicked, this, [this]() {
        auto *item = m_projectList ? m_projectList->currentItem() : nullptr;
        if (!item) return;
        const QString directory = item->data(Qt::UserRole).toString();
        m_recoverProjectButton->setEnabled(false);
        m_projectFeedback->setText(QStringLiteral("Checking finalized media fragments..."));
        QThread *thread = QThread::create([this, directory]() {
            const ProjectRecoveryResult result = m_projectStore->recover(directory, []() {
                return QThread::currentThread()->isInterruptionRequested();
            });
            QMetaObject::invokeMethod(this, [this, directory, result]() {
                refreshProjectList();
                if (!result.ok()) {
                    m_projectFeedback->setText(result.error);
                    return;
                }
                if (m_recordingStatus) {
                    m_recordingStatus->setText(QStringLiteral(
                        "Recovered %1 media fragment(s); preserved %2 incomplete fragment(s)")
                        .arg(result.usableMediaFiles).arg(result.quarantinedMediaFiles));
                }
                openProjectDirectory(directory);
            }, Qt::QueuedConnection);
        });
        m_previewThreads.append(thread);
        connect(thread, &QThread::finished, this,
                [this, thread]() { m_previewThreads.removeAll(thread); });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    });
    actions->addWidget(m_recoverProjectButton);
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
    layout->setContentsMargins(36, 28, 36, 30);
    layout->setSpacing(14);
    auto *heading = new QLabel(QStringLiteral("Activity"), page);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);

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
    layout->setContentsMargins(36, 28, 36, 30);
    layout->setSpacing(16);
    auto *heading = new QLabel(QStringLiteral("Settings"), container);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
    layout->addWidget(mutedLabel(
        QStringLiteral("Connection, privacy, recording quality, and local support tools."),
        container));

    auto *receiverCard = card(container);
    auto *receiverLayout = new QVBoxLayout(receiverCard);
    receiverLayout->setContentsMargins(22, 22, 22, 22);
    auto *receiverTitle = new QLabel(QStringLiteral("Connection"), receiverCard);
    receiverTitle->setObjectName(QStringLiteral("cardTitle"));
    receiverLayout->addWidget(receiverTitle);
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
    auto *securityTitle = new QLabel(QStringLiteral("Shared Wi-Fi protection"), securityCard);
    securityTitle->setObjectName(QStringLiteral("cardTitle"));
    securityLayout->addWidget(securityTitle);
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
    auto *appTitle = new QLabel(QStringLiteral("App behavior"), appCard);
    appTitle->setObjectName(QStringLiteral("cardTitle"));
    appLayout->addWidget(appTitle);
    m_bluetoothCheck = new QCheckBox(QStringLiteral("Enable Bluetooth discovery fallback"), appCard);
    m_autostartCheck = new QCheckBox(QStringLiteral("Start UxPlay Studio when I sign in"), appCard);
    m_notificationsCheck = new QCheckBox(QStringLiteral("Show connection notifications"), appCard);
    appLayout->addWidget(m_bluetoothCheck);
    appLayout->addWidget(m_autostartCheck);
    appLayout->addWidget(m_notificationsCheck);
    layout->addWidget(appCard);

    auto *supportCard = card(container);
    auto *supportLayout = new QHBoxLayout(supportCard);
    supportLayout->setContentsMargins(22, 18, 22, 18);
    auto *supportCopy = new QVBoxLayout;
    auto *supportTitle = new QLabel(QStringLiteral("Activity & diagnostics"), supportCard);
    supportTitle->setObjectName(QStringLiteral("cardTitle"));
    supportCopy->addWidget(supportTitle);
    supportCopy->addWidget(mutedLabel(
        QStringLiteral("Inspect local receiver events or copy a privacy-safe system report."),
        supportCard));
    supportLayout->addLayout(supportCopy, 1);
    auto *activity = new QPushButton(QStringLiteral("Activity log"), supportCard);
    activity->setObjectName(QStringLiteral("secondaryButton"));
    connect(activity, &QPushButton::clicked, this, [this]() { selectPage(2); });
    supportLayout->addWidget(activity);
    auto *diagnostics = new QPushButton(QStringLiteral("Diagnostics"), supportCard);
    diagnostics->setObjectName(QStringLiteral("secondaryButton"));
    connect(diagnostics, &QPushButton::clicked, this, [this]() { selectPage(4); });
    supportLayout->addWidget(diagnostics);
    layout->addWidget(supportCard);

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
    layout->setContentsMargins(36, 28, 36, 30);
    layout->setSpacing(14);
    auto *heading = new QLabel(QStringLiteral("Diagnostics"), page);
    heading->setObjectName(QStringLiteral("sectionTitle"));
    layout->addWidget(heading);
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

bool MainWindow::restartReceiver() {
    if (!ensureBonjourAvailable()) {
        appendActivity(QStringLiteral("Settings"),
                       QStringLiteral("Receiver restart was blocked because Bonjour Service is unavailable."));
        return false;
    }
    stopBluetoothBeacon();
    startBluetoothBeacon();
    appendActivity(QStringLiteral("Receiver"), QStringLiteral("Restarting receiver…"));
    m_engine->restart(m_config, m_videoSurface->nativeHandle(), bleStatusPath());
    return true;
}

void MainWindow::toggleReceiver() {
    if (receiverToggleStops(m_engine->state())) {
        stopReceiver();
    } else {
        startReceiver();
    }
}

void MainWindow::handleStateChanged(ReceiverState state) {
    m_statusBadge->setText(receiverStateLabel(state));
    QString tone = QStringLiteral("idle");
    if (state == ReceiverState::Ready || state == ReceiverState::Connecting)
        tone = QStringLiteral("ready");
    else if (state == ReceiverState::Mirroring)
        tone = QStringLiteral("live");
    else if (state == ReceiverState::Retrying || state == ReceiverState::Error)
        tone = QStringLiteral("warning");
    m_statusBadge->setProperty("tone", tone);
    refreshStyle(m_statusBadge);
    const bool running = receiverToggleStops(state);
    m_receiverToggle->setText(running ? QStringLiteral("Stop receiver")
                                      : QStringLiteral("Start receiver"));
    m_receiverToggle->setProperty("running", running);
    refreshStyle(m_receiverToggle);
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
    refreshLayerList();
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
        if (event.message.contains(QStringLiteral("recording"), Qt::CaseInsensitive) &&
            m_recordingSession) {
            m_recordingSession->markAirplayFailure(event.message);
            if (m_recordingStatus) m_recordingStatus->setText(m_recordingSession->statusSummary());
        }
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
    if (m_pages && m_pages->currentIndex() == 0 && m_pageTitle)
        m_pageTitle->setText(m_config.receiverName);
    updateSecuritySummary();
    refreshDiagnostics();
    appendActivity(QStringLiteral("Settings"), QStringLiteral("Receiver settings were updated."));
    if (m_engine->isRunning()) {
        if (restartReceiver()) {
            m_settingsFeedback->setText(
                QStringLiteral("Saved. Restarting the receiver with the new settings."));
            m_settingsFeedback->setStyleSheet(QStringLiteral("color: #38d996;"));
        } else {
            m_settingsFeedback->setText(
                QStringLiteral("Saved locally, but the receiver could not restart."));
            m_settingsFeedback->setStyleSheet(QStringLiteral("color: #ff6b7a;"));
        }
        return;
    }
    m_settingsFeedback->setText(
        QStringLiteral("Saved. Start the receiver to apply the new settings."));
    m_settingsFeedback->setStyleSheet(QStringLiteral("color: #38d996;"));
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
    connect(m_beacon, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (!m_beacon) return;
        appendActivity(QStringLiteral("Bluetooth"),
                       QStringLiteral("Bluetooth helper failed to start: %1")
                           .arg(m_beacon->errorString()));
    });
    connect(m_beacon,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                if (m_quitting || !m_beacon) return;
                if (exitStatus == QProcess::CrashExit || exitCode != 0) {
                    appendActivity(QStringLiteral("Bluetooth"),
                                   QStringLiteral("Bluetooth helper stopped unexpectedly (exit %1)")
                                       .arg(exitCode));
                }
            });
    connect(m_beacon, &QProcess::readyRead, this, [this]() {
        const QString text = QString::fromLocal8Bit(m_beacon->readAll()).trimmed();
        if (!text.isEmpty()) appendActivity(QStringLiteral("Bluetooth"), text);
    });
    m_beacon->start(executable, {QStringLiteral("--path"), bleStatusPath()});
    if (!m_beacon->waitForStarted(1000)) {
        appendActivity(QStringLiteral("Bluetooth"),
                       QStringLiteral("Bluetooth helper did not start: %1")
                           .arg(m_beacon->errorString()));
        delete m_beacon;
        m_beacon = nullptr;
    }
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
    if (m_stageFooter) m_stageFooter->hide();
    if (m_stageLayout) {
        m_stageLayout->setContentsMargins(0, 0, 0, 0);
        m_stageLayout->setSpacing(0);
    }
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
    m_playerPageLayout->setContentsMargins(0, 0, 0, 0);
    m_playerPageLayout->setSpacing(0);
    m_playerLayout->setContentsMargins(0, 0, 0, 0);
    m_playerLayout->setSpacing(0);
    refreshStyle(m_playerPage);
    refreshStyle(m_playerCard);
    refreshStyle(m_videoSurface);

    m_sidebar->show();
    m_header->show();
    m_sessionPanel->show();
    m_playerChrome->show();
    m_playerControls->show();
    if (m_stageFooter) m_stageFooter->show();
    if (m_stageLayout) {
        m_stageLayout->setContentsMargins(20, 18, 20, 12);
        m_stageLayout->setSpacing(8);
    }
    m_fullscreenButton->setText(QString());

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

bool MainWindow::hasActiveRecordingWork() const {
    if (!m_recordingSession) return false;
    switch (m_recordingSession->state()) {
    case RecordingState::Starting:
    case RecordingState::Recording:
    case RecordingState::Finalizing:
        return true;
    case RecordingState::Idle:
    case RecordingState::Failed:
        return false;
    }
    return false;
}

void MainWindow::showBlockedExitFeedback(const QString &statusMessage,
                                         const QString &activityMessage) {
    if (m_recordingStatus) m_recordingStatus->setText(statusMessage);
    appendActivity(QStringLiteral("Recording"), activityMessage);
}

void MainWindow::quitApplication() {
    if (hasActiveRecordingWork()) {
        showBlockedExitFeedback(
            QStringLiteral("Stop recording before quitting UxPlay Studio"),
            QStringLiteral("Quit was blocked because recording media is still active."));
        return;
    }
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
    if (hasActiveRecordingWork()) {
        showBlockedExitFeedback(
            QStringLiteral("Stop recording before closing UxPlay Studio"),
            QStringLiteral("Close was blocked because recording media is still active."));
        event->ignore();
        return;
    }

    // Closing the main window must end the process. Keeping a hidden receiver
    // alive made the desktop shortcut reactivate a stale native video surface,
    // which could reopen as an unpainted white window after a long idle period.
    m_quitting = true;
    stopBluetoothBeacon();
    m_engine->stop();
    if (m_tray) {
        m_tray->hide();
    }
    event->accept();
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
