#include "mainwindow.h"
#include "projects/projectstore.h"

#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

#include <algorithm>

class MainWindowTest final : public QObject {
    Q_OBJECT

private slots:
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
        auto *rendererBadge = window.findChild<QLabel *>(QStringLiteral("miniBadge"));
        auto *diagnostics = window.findChild<QPlainTextEdit *>(QStringLiteral("diagnosticsText"));

        QVERIFY(button);
        QVERIFY(sidebar);
        QVERIFY(header);
        QVERIFY(chrome);
        QVERIFY(controls);
        QVERIFY(card);
        QVERIFY(pageLayout);
        QVERIFY(playerLayout);
        QVERIFY(rendererBadge);
        QVERIFY(diagnostics);
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
        QVERIFY(card->property("fullscreen").toBool());
        QCOMPARE(pageLayout->contentsMargins(), QMargins());
        QCOMPARE(playerLayout->contentsMargins(), QMargins());
        QCOMPARE(QApplication::overrideCursor()->shape(), Qt::BlankCursor);

        QTest::keyClick(&window, Qt::Key_Escape);
        QTRY_VERIFY(!window.isFullScreen());
        QVERIFY(sidebar->isVisible());
        QVERIFY(header->isVisible());
        QVERIFY(chrome->isVisible());
        QVERIFY(controls->isVisible());
        QVERIFY(!card->property("fullscreen").toBool());
        QCOMPARE(pageLayout->contentsMargins(), QMargins(30, 24, 30, 30));
        QCOMPARE(playerLayout->contentsMargins(), QMargins(18, 18, 18, 16));
        QVERIFY(QApplication::overrideCursor() == nullptr);
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

    void projectsPageExposesAndRecoversInterruptedProjects() {
        QTemporaryDir projects;
        ProjectStore store(QDir(projects.path()).filePath(QStringLiteral("UxPlay Studio")));
        SceneDocument document;
        document.setTitle(QStringLiteral("Interrupted class"));
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());

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
        QTest::mouseClick(recoverButton, Qt::LeftButton);
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
    }
};

QTEST_MAIN(MainWindowTest)
#include "test_mainwindow.moc"
