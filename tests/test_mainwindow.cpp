#include "mainwindow.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QtTest>

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

        QVERIFY(button);
        QVERIFY(sidebar);
        QVERIFY(header);
        QVERIFY(chrome);
        QVERIFY(controls);
        QVERIFY(card);
        QVERIFY(pageLayout);
        QVERIFY(playerLayout);

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
};

QTEST_MAIN(MainWindowTest)
#include "test_mainwindow.moc"
