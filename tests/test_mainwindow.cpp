#include "mainwindow.h"
#include "projects/projectstore.h"
#include "recording/recordingsession.h"
#include "studio/cameraselfview.h"
#include "studio/scenecanvas.h"
#include "uxplay_api.h"

#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
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

    void fullscreenShowsOnlyTheVideoSurfaceAndRestoresChrome() {
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
        selfView->setFrame(QImage(640, 360, QImage::Format_ARGB32));
        selfView->setActive(true);
        QVERIFY(selfView->isVisible());
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
        QVERIFY(selfView->isVisible());
        QVERIFY(selfView->isActive());
        QVERIFY(!selfView->isWindow());
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
        QVERIFY(selfView->isVisible());
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
        auto *controls = window.findChild<QWidget *>(QStringLiteral("playerControls"));
        auto *record = window.findChild<QPushButton *>(QStringLiteral("recordButton"));
        auto *status = window.findChild<QLabel *>(QStringLiteral("statusBadge"));
        QVERIFY(sidebar);
        QVERIFY(sessionPanel);
        QVERIFY(controls);
        QVERIFY(record);
        QVERIFY(status);
        QCOMPARE(sidebar->width(), 112);
        QVERIFY(sessionPanel->maximumWidth() <= 348);
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
        QVERIFY(controls->minimumSizeHint().width() <= controls->width());
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
        QVERIFY(session->start(created.project, {}));
        QTRY_COMPARE(session->state(), RecordingState::Recording);

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
