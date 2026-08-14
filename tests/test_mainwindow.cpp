#include "mainwindow.h"
#include "projects/projectstore.h"
#include "recording/recordingsession.h"
#include "studio/cameraselfview.h"
#include "studio/scenecanvas.h"
#include "uxplay_api.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>
#include <gst/gst.h>

#include <algorithm>

namespace {
bool writePlayableVideoFragment(const QString &path, QString *error) {
    QString escapedPath = QDir::fromNativeSeparators(path);
    escapedPath.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    const QString description = QStringLiteral(
        "videotestsrc num-buffers=6 ! video/x-raw,width=160,height=90,framerate=30/1 "
        "! videoconvert ! openh264enc bitrate=200000 ! h264parse ! matroskamux "
        "! filesink location=\"%1\"").arg(escapedPath);
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!pipeline || parseError) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message)
                                      : QStringLiteral("Could not create media test pipeline");
        if (parseError) g_error_free(parseError);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (!ok && error) *error = QStringLiteral("Media test pipeline did not finish");
    if (message) gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ok;
}
}

class MainWindowTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);
        uxplay_set_recording_test_mode(1);
    }

    void cleanup() { uxplay_set_recording_test_runtime_failure(0); }

    void cleanupTestCase() { uxplay_set_recording_test_mode(0); }

    void fullscreenShowsOnlyTheComposedSceneAndRestoresChrome() {
        MainWindow window(nullptr, false);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *button = window.findChild<QPushButton *>(QStringLiteral("fullscreenButton"));
        auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
        auto *header = window.findChild<QWidget *>(QStringLiteral("header"));
        auto *chrome = window.findChild<QWidget *>(QStringLiteral("playerChrome"));
        auto *controls = window.findChild<QWidget *>(QStringLiteral("playerControls"));
        auto *card = window.findChild<QWidget *>(QStringLiteral("playerCard"));
        auto *pageLayout = window.findChild<QHBoxLayout *>(QStringLiteral("playerPageLayout"));
        auto *playerLayout = window.findChild<QVBoxLayout *>(QStringLiteral("playerLayout"));
        auto *stageLayout = window.findChild<QVBoxLayout *>(QStringLiteral("stageLayout"));
        auto *stageHint = window.findChild<QLabel *>(QStringLiteral("stageHint"));
        auto *rendererBadge = window.findChild<QLabel *>(QStringLiteral("miniBadge"));
        auto *diagnostics = window.findChild<QPlainTextEdit *>(QStringLiteral("diagnosticsText"));
        auto *selfView = window.findChild<CameraSelfView *>(QStringLiteral("cameraSelfView"));
        auto *sceneCanvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));

        QVERIFY(button);
        QVERIFY(sidebar);
        QVERIFY(header);
        QVERIFY(chrome);
        QVERIFY(controls);
        QVERIFY(card);
        QVERIFY(pageLayout);
        QVERIFY(playerLayout);
        QVERIFY(stageLayout);
        QVERIFY(stageHint);
        QVERIFY(rendererBadge);
        QVERIFY(diagnostics);
        QVERIFY(selfView);
        QVERIFY(sceneCanvas);
        selfView->setFrame(QImage(640, 360, QImage::Format_ARGB32));
        selfView->setActive(true);
        QVERIFY(!selfView->isVisible());
        QCOMPARE(rendererBadge->text(), QStringLiteral("D3D11 · EMBEDDED"));
        QVERIFY(diagnostics->toPlainText().contains(
            QStringLiteral("Decoder: Automatic; hardware preferred with software fallback")));
        QVERIFY(!diagnostics->toPlainText().contains(QStringLiteral("zero-copy"),
                                                      Qt::CaseInsensitive));

        QTest::mouseClick(button, Qt::LeftButton);
        QTRY_VERIFY(window.isFullScreen());
        QVERIFY(!sidebar->isVisible());
        QVERIFY(!header->isVisible());
        QVERIFY(!chrome->isVisible());
        QVERIFY(!controls->isVisible());
        QVERIFY(!selfView->isVisible());
        QVERIFY(selfView->isActive());
        QVERIFY(!selfView->isWindow());
        QVERIFY(sceneCanvas->presentationMode());
        QVERIFY(card->property("fullscreen").toBool());
        QCOMPARE(pageLayout->contentsMargins(), QMargins());
        QCOMPARE(playerLayout->contentsMargins(), QMargins());
        QCOMPARE(stageLayout->contentsMargins(), QMargins());
        QVERIFY(!stageHint->isVisible());
        QCOMPARE(QApplication::overrideCursor()->shape(), Qt::BlankCursor);

        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!window.isFullScreen());
        QVERIFY(sidebar->isVisible());
        QVERIFY(header->isVisible());
        QVERIFY(chrome->isVisible());
        QVERIFY(controls->isVisible());
        QVERIFY(!selfView->isVisible());
        QVERIFY(!sceneCanvas->presentationMode());
        QVERIFY(!card->property("fullscreen").toBool());
        QCOMPARE(pageLayout->contentsMargins(), QMargins());
        QCOMPARE(playerLayout->contentsMargins(), QMargins());
        QCOMPARE(stageLayout->contentsMargins(), QMargins(20, 18, 20, 12));
        QVERIFY(stageHint->isVisible());
        QVERIFY(QApplication::overrideCursor() == nullptr);
    }

    void modernShellPrioritizesTheStageAndKeepsPrimaryActionsReachable() {
        MainWindow window(nullptr, false);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *sidebar = window.findChild<QWidget *>(QStringLiteral("sidebar"));
        auto *sessionPanel = window.findChild<QWidget *>(QStringLiteral("studioDock"));
        auto *splitter = window.findChild<QSplitter *>(QStringLiteral("playerSplitter"));
        auto *controls = window.findChild<QWidget *>(QStringLiteral("playerControls"));
        auto *record = window.findChild<QPushButton *>(QStringLiteral("recordButton"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("statusBadge"));
        QVERIFY(sidebar);
        QVERIFY(sessionPanel);
        QVERIFY(splitter);
        QVERIFY(controls);
        QVERIFY(record);
        QVERIFY(status);
        QCOMPARE(sidebar->width(), 112);
        QCOMPARE(splitter->count(), 2);
        QVERIFY(sessionPanel->minimumWidth() >= 280);
        QVERIFY(sessionPanel->maximumWidth() >= 500);
        QCOMPARE(record->parentWidget(), controls);
        QCOMPARE(window.findChildren<QPushButton *>(QStringLiteral("navButton")).size(), 3);
        QCOMPARE(window.findChildren<QWidget *>(QStringLiteral("connectStep")).size(), 3);
        QVERIFY(status->styleSheet().isEmpty());
        window.resize(1024, 680);
        QCoreApplication::processEvents();
        QVERIFY(controls->minimumSizeHint().width() <= controls->width());
        QPushButton *edit = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
        QVERIFY(edit);
        QTest::mouseClick(edit, Qt::LeftButton);
        QCoreApplication::processEvents();
        auto *selfView = window.findChild<CameraSelfView *>(QStringLiteral("cameraSelfView"));
        QVERIFY(!selfView->isVisible());
        selfView->setFrame(QImage(320, 180, QImage::Format_ARGB32));
        selfView->setActive(true);
        QVERIFY(!selfView->isVisible());
        QTest::mouseClick(edit, Qt::LeftButton);
        QCoreApplication::processEvents();
        QVERIFY(controls->minimumSizeHint().width() <= controls->width());
        const int originalDockWidth = splitter->sizes().value(1);
        splitter->setSizes({qMax(1, splitter->width() - 500), 500});
        QCoreApplication::processEvents();
        QVERIFY(splitter->sizes().value(1) > originalDockWidth);
        auto *layers = window.findChild<QListWidget *>(QStringLiteral("layerList"));
        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        QVERIFY(layers);
        QVERIFY(canvas);
        layers->setCurrentRow(0);
        QDoubleSpinBox *xField = nullptr;
        for (QDoubleSpinBox *field : window.findChildren<QDoubleSpinBox *>())
            if (field->accessibleName() == QStringLiteral("layerTransformX")) xField = field;
        QVERIFY(xField);
        QVERIFY(xField->isEnabled());
        const QString layerId = layers->item(0)->data(Qt::UserRole).toString();
        xField->setValue(120.0);
        QCOMPARE(canvas->document()->layer(SceneFormat::Wide, layerId)->transform.frame.x(), 120.0);
        QVERIFY(canvas->undoStack()->canUndo());
    }

    void dockListsAndCanvasViewHaveVisibleResizeAndZoomControls() {
        MainWindow window(nullptr, false);
        window.resize(1440, 880);
        window.show();
        QTRY_VERIFY(window.isVisible());

        QPushButton *edit = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
        QVERIFY(edit);
        QTest::mouseClick(edit, Qt::LeftButton);
        QCoreApplication::processEvents();

        auto *sourceSplitter = window.findChild<QSplitter *>(QStringLiteral("sourceAreaSplitter"));
        auto *layerSplitter = window.findChild<QSplitter *>(QStringLiteral("layerAreaSplitter"));
        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        auto *zoomOut = window.findChild<QPushButton *>(QStringLiteral("zoomOutButton"));
        auto *fitCanvas = window.findChild<QPushButton *>(QStringLiteral("fitCanvasButton"));
        auto *zoomLabel = window.findChild<QLabel *>(QStringLiteral("zoomLabel"));
        auto *hint = window.findChild<QLabel *>(QStringLiteral("stageHint"));
        QVERIFY(sourceSplitter);
        QVERIFY(layerSplitter);
        QVERIFY(canvas);
        QVERIFY(zoomOut);
        QVERIFY(fitCanvas);
        QVERIFY(zoomLabel);
        QVERIFY(hint);
        QCOMPARE(sourceSplitter->orientation(), Qt::Vertical);
        QCOMPARE(layerSplitter->orientation(), Qt::Vertical);
        QCOMPARE(sourceSplitter->count(), 2);
        QCOMPARE(layerSplitter->count(), 2);
        auto *sources = window.findChild<QListWidget *>(QStringLiteral("sourceList"));
        QVERIFY(sources);
        QVERIFY(sources->item(0)->text().contains(QStringLiteral("OFFLINE")));

        const QList<int> sourceBefore = sourceSplitter->sizes();
        QVERIFY(sourceBefore.at(0) <= 260);
        sourceSplitter->setSizes({sourceBefore.at(0) + 48, qMax(1, sourceBefore.at(1) - 48)});
        QCoreApplication::processEvents();
        QVERIFY(sourceSplitter->sizes().at(0) > sourceBefore.at(0));

        const QList<int> layerBefore = layerSplitter->sizes();
        QVERIFY(layerBefore.at(0) >= 120);
        const int layerDelta = layerBefore.at(0) > 120 ? -48 : 48;
        layerSplitter->setSizes({qMax(1, layerBefore.at(0) + layerDelta),
                                 qMax(1, layerBefore.at(1) - layerDelta)});
        QCoreApplication::processEvents();
        if (layerDelta < 0)
            QVERIFY(layerSplitter->sizes().at(0) < layerBefore.at(0));
        else
            QVERIFY(layerSplitter->sizes().at(0) > layerBefore.at(0));

        QCOMPARE(canvas->zoomPercent(), 100);
        QVERIFY(hint->text().contains(QStringLiteral("Canvas 1920 × 1080")));
        QTest::mouseClick(zoomOut, Qt::LeftButton);
        QVERIFY(canvas->zoomPercent() < 100);
        QCOMPARE(zoomLabel->text(), QStringLiteral("%1%").arg(canvas->zoomPercent()));
        QTest::mouseClick(fitCanvas, Qt::LeftButton);
        QCOMPARE(canvas->zoomPercent(), 100);
        QCOMPARE(zoomLabel->text(), QStringLiteral("100%"));
    }

    void workspaceUsesOneComposedCanvasAndExclusiveFormatTabs() {
        MainWindow window(nullptr, false);
        window.resize(1440, 880);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        auto *stack = window.findChild<QStackedWidget *>(QStringLiteral("previewStack"));
        QVERIFY(canvas);
        QVERIFY(stack);
        QCOMPARE(stack->currentWidget(), static_cast<QWidget *>(canvas));

        QPushButton *edit = nullptr;
        QPushButton *wide = nullptr;
        QPushButton *vertical = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
            if (button->text() == QStringLiteral("16:9")) wide = button;
            if (button->text() == QStringLiteral("9:16")) vertical = button;
            QVERIFY(button->text() != QStringLiteral("Live"));
        }
        QVERIFY(edit);
        QVERIFY(wide);
        QVERIFY(vertical);
        QVERIFY(edit->isChecked());
        QVERIFY(wide->isChecked());
        QVERIFY(!vertical->isChecked());
        QCOMPARE(canvas->format(), SceneFormat::Wide);

        QTest::mouseClick(vertical, Qt::LeftButton);
        QCoreApplication::processEvents();
        QCOMPARE(canvas->format(), SceneFormat::Vertical);
        QVERIFY(vertical->isChecked());
        QVERIFY(!wide->isChecked());
        QVERIFY(canvas->canvasSize() == QSize(1080, 1920));

        QTest::mouseClick(wide, Qt::LeftButton);
        QCoreApplication::processEvents();
        QCOMPARE(canvas->format(), SceneFormat::Wide);
        QVERIFY(wide->isChecked());
        QVERIFY(!vertical->isChecked());
    }

    void cameraMonitorStaysHiddenInUnifiedWorkspaceWhenFramesArrive() {
        MainWindow window(nullptr, false);
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *edit = window.findChild<QPushButton *>(QStringLiteral("segmentedButton"));
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
        }
        auto *selfView = window.findChild<CameraSelfView *>(QStringLiteral("cameraSelfView"));
        QVERIFY(edit);
        QVERIFY(selfView);

        QTest::mouseClick(edit, Qt::LeftButton);
        selfView->setActive(true);
        selfView->setFrame(QImage(320, 180, QImage::Format_ARGB32));
        QCoreApplication::processEvents();
        QVERIFY(selfView->isActive());
        QVERIFY(!selfView->isVisible());
    }

    void contextMenusAreAvailableAcrossCanvasAndLists() {
        MainWindow window(nullptr, false);
        window.resize(1440, 880);
        window.show();
        QTRY_VERIFY(window.isVisible());

        QPushButton *edit = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
        QVERIFY(edit);
        QTest::mouseClick(edit, Qt::LeftButton);
        QCoreApplication::processEvents();

        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        auto *layers = window.findChild<QListWidget *>(QStringLiteral("layerList"));
        auto *sources = window.findChild<QListWidget *>(QStringLiteral("sourceList"));
        QVERIFY(canvas);
        QVERIFY(layers);
        QVERIFY(sources);

        const auto openAndClose = [&](QWidget *target, const QString &menuName, const QPoint &local) {
            bool seen = false;
            QTimer::singleShot(0, [&seen, menuName]() {
                auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
                if (menu && menu->objectName() == menuName) {
                    seen = true;
                    menu->close();
                }
            });
            const QPoint global = target->mapToGlobal(local);
            QContextMenuEvent event(QContextMenuEvent::Mouse, local, global);
            QCoreApplication::sendEvent(target, &event);
            QVERIFY2(seen, qPrintable(QStringLiteral("Context menu did not open: %1").arg(menuName)));
        };

        openAndClose(canvas->viewport(), QStringLiteral("layerContextMenu"),
                     canvas->viewport()->rect().center());
        openAndClose(canvas->viewport(), QStringLiteral("canvasContextMenu"), QPoint(4, 4));
        openAndClose(layers->viewport(), QStringLiteral("layerContextMenu"),
                     layers->visualItemRect(layers->item(0)).center());
        openAndClose(sources->viewport(), QStringLiteral("sourceContextMenu"),
                     sources->visualItemRect(sources->item(0)).center());

        auto *selfView = window.findChild<CameraSelfView *>(QStringLiteral("cameraSelfView"));
        QVERIFY(selfView);
        QVERIFY(!selfView->isVisible());

        QPushButton *projectsButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
        }
        QVERIFY(projectsButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        QCoreApplication::processEvents();
        auto *projects = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QVERIFY(projects);
        openAndClose(projects->viewport(), QStringLiteral("projectContextMenu"), QPoint(4, 4));
    }

    void inspectorReflectsSelectedLayerOpacityAndMask() {
        MainWindow window(nullptr, false);
        window.show();
        QTRY_VERIFY(window.isVisible());

        QPushButton *edit = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->text() == QStringLiteral("Edit layout")) edit = button;
        QVERIFY(edit);
        QTest::mouseClick(edit, Qt::LeftButton);
        QCoreApplication::processEvents();

        auto *layers = window.findChild<QListWidget *>(QStringLiteral("layerList"));
        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        QVERIFY(layers);
        QVERIFY(canvas);
        QVERIFY(layers->count() > 0);

        const QString layerId = layers->item(0)->data(Qt::UserRole).toString();
        const SceneLayer *layer = canvas->document()->layer(SceneFormat::Wide, layerId);
        QVERIFY(layer);
        SceneTransform transform = layer->transform;
        transform.opacity = 0.75;
        transform.mask = SceneMask::Circle;
        canvas->document()->setTransform(SceneFormat::Wide, layerId, transform);
        canvas->refreshFromDocument();

        QComboBox *opacity = nullptr;
        QComboBox *mask = nullptr;
        for (QComboBox *combo : window.findChildren<QComboBox *>()) {
            if (combo->accessibleName() == QStringLiteral("layerOpacity")) opacity = combo;
            if (combo->accessibleName() == QStringLiteral("layerMask")) mask = combo;
        }
        QVERIFY(opacity);
        QVERIFY(mask);

        layers->setCurrentRow(0);
        QCoreApplication::processEvents();

        QCOMPARE(opacity->currentData().toDouble(), 0.75);
        QCOMPARE(mask->currentData().toInt(), static_cast<int>(SceneMask::Circle));
    }

    void deleteButtonAndShortcutsRemoveOnlyUnlockedLayers() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        const QString airplay = document.addSource(SceneSourceType::AirPlay, QStringLiteral("iPad"));
        const QString title = document.addSource(SceneSourceType::Text, QStringLiteral("Title"));
        const QString color = document.addSource(SceneSourceType::Color, QStringLiteral("Backdrop"));
        document.addLayer(SceneFormat::Wide, airplay);
        document.addLayer(SceneFormat::Wide, title);
        document.addLayer(SceneFormat::Wide, color);
        const auto created = store.create(document);
        QVERIFY(created.ok());

        MainWindow window(nullptr, false, projects.path());
        window.show();
        QTRY_VERIFY(window.isVisible());
        QPushButton *projectsButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
        }
        QVERIFY(projectsButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        auto *projectList = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QPushButton *openButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->accessibleName() == QStringLiteral("openProjectButton")) openButton = button;
        }
        QVERIFY(projectList);
        QVERIFY(openButton);
        projectList->setCurrentRow(0);
        QTest::mouseClick(openButton, Qt::LeftButton);

        auto *layers = window.findChild<QListWidget *>(QStringLiteral("layerList"));
        QPushButton *deleteButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->accessibleName() == QStringLiteral("deleteLayerButton")) deleteButton = button;
        }
        QVERIFY(layers);
        QVERIFY(deleteButton);
        QCOMPARE(layers->count(), 3);
        layers->setCurrentRow(0);
        QTest::mouseClick(deleteButton, Qt::LeftButton);
        QCOMPARE(layers->count(), 2);

        layers->setCurrentRow(0);
        QTest::keyClick(layers, Qt::Key_Delete);
        QCOMPARE(layers->count(), 1);

        layers->setCurrentRow(0);
        QPushButton *lockButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->toolTip() == QStringLiteral("Lock or unlock layer")) lockButton = button;
        }
        QVERIFY(lockButton);
        QTest::mouseClick(lockButton, Qt::LeftButton);
        QTest::keyClick(layers, Qt::Key_Backspace);
        QCOMPARE(layers->count(), 1);
        QVERIFY(store.load(created.project.directory).document->composition(SceneFormat::Wide).layers.size() == 1);
    }

    void closingTheMainWindowDoesNotLeaveATrayOnlyInstance() {
        const bool previousQuitOnLastWindow = QApplication::quitOnLastWindowClosed();
        QApplication::setQuitOnLastWindowClosed(false);
        auto *window = new MainWindow(nullptr, false);
        window->setAttribute(Qt::WA_DeleteOnClose);
        window->show();
        QTRY_VERIFY(window->isVisible());

        QSignalSpy destroyed(window, &QObject::destroyed);
        QVERIFY(window->close());
        QTRY_COMPARE(destroyed.count(), 1);

        QApplication::setQuitOnLastWindowClosed(previousQuitOnLastWindow);
    }

    void closingWhileRecordingKeepsTheWindowOpen() {
        QTemporaryDir projects;
        const QString root = QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio"));
        ProjectStore store(root);
        const auto created = store.create(SceneDocument{});
        QVERIFY(created.ok());

        MainWindow window(nullptr, false, projects.path());
        window.show();
        QTRY_VERIFY(window.isVisible());

        auto *session = window.findChild<RecordingSession *>();
        QVERIFY(session);
        auto *canvas = window.findChild<SceneCanvas *>(QStringLiteral("sceneCanvas"));
        auto *editButton = window.findChild<QPushButton *>(QStringLiteral("segmentedButton"));
        QVERIFY(canvas);
        QVERIFY(editButton);
        QVERIFY(session->start(created.project, {}));
        QTRY_COMPARE(session->state(), RecordingState::Recording);
        QVERIFY(!canvas->editingEnabled());
        QVERIFY(!editButton->isEnabled());

        QSignalSpy destroyed(&window, &QObject::destroyed);
        window.close();
        QCoreApplication::processEvents();

        QVERIFY(window.isVisible());
        QCOMPARE(destroyed.count(), 0);
        const auto labels = window.findChildren<QLabel *>(QStringLiteral("mutedLabel"));
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
            return label->text().contains(
                QStringLiteral("Stop recording before closing UxPlay Studio"));
        }));
        QVERIFY(session->stop());
        QTRY_VERIFY(canvas->editingEnabled());
    }

    void recordAndExportAreGatedWithActionableFeedback() {
        QTemporaryDir projects;
        MainWindow window(nullptr, false, projects.path());
        auto *record = window.findChild<QPushButton *>(QStringLiteral("recordButton"));
        QVERIFY(record);
        QTest::mouseClick(record, Qt::LeftButton);
        auto labels = window.findChildren<QLabel *>(QStringLiteral("mutedLabel"));
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
            return label->text().contains(QStringLiteral("Connect an iPad"));
        }));
        QPushButton *exportButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>())
            if (button->accessibleName() == QStringLiteral("exportButton")) exportButton = button;
        QVERIFY(exportButton);
        QTest::mouseClick(exportButton, Qt::LeftButton);
        labels = window.findChildren<QLabel *>(QStringLiteral("mutedLabel"));
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
            return label->text().contains(QStringLiteral("Record or open a project"));
        }));
    }

    void recordingRuntimeFailureUpdatesTheVisibleStatus() {
        QTemporaryDir projects;
        const QString root = QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio"));
        ProjectStore store(root);
        const auto created = store.create(SceneDocument{});
        QVERIFY(created.ok());
        MainWindow window(nullptr, false, projects.path());
        auto *session = window.findChild<RecordingSession *>();
        QVERIFY(session);
        QLabel *status = nullptr;
        for (QLabel *label : window.findChildren<QLabel *>(QStringLiteral("mutedLabel"))) {
            if (label->text() == QStringLiteral("Ready to record")) status = label;
        }
        QVERIFY(status);
        QVERIFY(session->start(created.project, {}));

        uxplay_set_recording_test_runtime_failure(1);
        QTRY_VERIFY_WITH_TIMEOUT(
            status->text().contains(QStringLiteral("Recording issue")), 1000);
        QVERIFY(!session->stop());
    }

    void projectsPageExposesAndRecoversInterruptedProjects() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        document.setTitle(QStringLiteral("Interrupted class"));
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());
        QString mediaError;
        QVERIFY2(writePlayableVideoFragment(
            QDir(created.project.airplayDirectory()).filePath(QStringLiteral("video-00000.mkv")),
            &mediaError), qPrintable(mediaError));

        MainWindow window(nullptr, false, projects.path());
        QPushButton *projectsButton = nullptr;
        QPushButton *recoverButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
            if (button->accessibleName() == QStringLiteral("recoverProjectButton")) recoverButton = button;
        }
        QVERIFY(projectsButton);
        QVERIFY(recoverButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        auto *list = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        QVERIFY(list->item(0)->text().contains(QStringLiteral("RECOVERABLE")));
        list->setCurrentRow(0);
        QTRY_VERIFY(recoverButton->isEnabled());
        QTest::mouseClick(recoverButton, Qt::LeftButton);
        QTRY_COMPARE_WITH_TIMEOUT(store.load(created.project.directory).project.state,
                                  ProjectState::Ready, 10000);
    }

    void recoveryFailsClosedAndExplainsMissingMedia() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        document.setTitle(QStringLiteral("Empty interrupted class"));
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());

        MainWindow window(nullptr, false, projects.path());
        QPushButton *projectsButton = nullptr;
        QPushButton *recoverButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
            if (button->accessibleName() == QStringLiteral("recoverProjectButton"))
                recoverButton = button;
        }
        QVERIFY(projectsButton);
        QVERIFY(recoverButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        auto *list = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        list->setCurrentRow(0);
        QTRY_VERIFY(recoverButton->isEnabled());
        QTest::mouseClick(recoverButton, Qt::LeftButton);
        auto *feedback = window.findChild<QLabel *>(QStringLiteral("projectFeedback"));
        QVERIFY(feedback);
        QTRY_VERIFY_WITH_TIMEOUT(feedback->text().contains(QStringLiteral("No usable AirPlay video")),
                                 10000);
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recoverable);
    }

    void recoverableProjectCannotBypassValidationThroughExport() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        document.setTitle(QStringLiteral("Interrupted export"));
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recoverable).isEmpty());

        MainWindow window(nullptr, false, projects.path());
        QPushButton *projectsButton = nullptr;
        QPushButton *openButton = nullptr;
        QPushButton *exportButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
            if (button->accessibleName() == QStringLiteral("openProjectButton")) openButton = button;
            if (button->accessibleName() == QStringLiteral("exportButton")) exportButton = button;
        }
        QVERIFY(projectsButton);
        QVERIFY(openButton);
        QVERIFY(exportButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        auto *list = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QVERIFY(list);
        QCOMPARE(list->count(), 1);
        list->setCurrentRow(0);
        QTest::mouseClick(openButton, Qt::LeftButton);
        QTest::mouseClick(exportButton, Qt::LeftButton);

        const auto labels = window.findChildren<QLabel *>(QStringLiteral("mutedLabel"));
        QVERIFY(std::any_of(labels.cbegin(), labels.cend(), [](QLabel *label) {
            return label->text().contains(QStringLiteral("Recover this project before export"));
        }));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recoverable);
    }

    void layerDragDropPersistsTheSceneOrder() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        const QString airplay = document.addSource(SceneSourceType::AirPlay,
                                                    QStringLiteral("iPad"));
        const QString title = document.addSource(SceneSourceType::Text,
                                                  QStringLiteral("Title"));
        document.addLayer(SceneFormat::Wide, airplay);
        document.addLayer(SceneFormat::Wide, title);
        const auto created = store.create(document);
        QVERIFY(created.ok());

        MainWindow window(nullptr, false, projects.path());
        QPushButton *projectsButton = nullptr;
        QPushButton *openButton = nullptr;
        for (QPushButton *button : window.findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Projects")) projectsButton = button;
            if (button->accessibleName() == QStringLiteral("openProjectButton")) openButton = button;
        }
        QVERIFY(projectsButton);
        QVERIFY(openButton);
        QTest::mouseClick(projectsButton, Qt::LeftButton);
        auto *projectsList = window.findChild<QListWidget *>(QStringLiteral("projectList"));
        QVERIFY(projectsList);
        projectsList->setCurrentRow(0);
        QTest::mouseClick(openButton, Qt::LeftButton);

        auto *layers = window.findChild<QListWidget *>(QStringLiteral("layerList"));
        QVERIFY(layers);
        QCOMPARE(layers->count(), 2);
        QCOMPARE(layers->item(0)->data(Qt::UserRole).toString(),
                 document.composition(SceneFormat::Wide).layers.at(1).id);
        QVERIFY(layers->model()->moveRow(QModelIndex(), 1, QModelIndex(), 0));

        const auto saved = store.load(created.project.directory);
        QVERIFY(saved.ok());
        const auto &savedLayers = saved.document->composition(SceneFormat::Wide).layers;
        QCOMPARE(savedLayers.size(), 2);
        QCOMPARE(savedLayers.at(0).sourceId, title);
        QCOMPARE(savedLayers.at(1).sourceId, airplay);
    }
};

QTEST_MAIN(MainWindowTest)
#include "test_mainwindow.moc"
