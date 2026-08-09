#include "videosurface.h"

#include <QtTest>

class VideoSurfaceTest final : public QObject {
    Q_OBJECT

private slots:
    void ownsStableChildWindowHandle() {
        QWidget host;
        VideoSurface surface(&host);
        host.show();
        QTest::qWait(20);
        const quintptr first = surface.nativeHandle();
        QVERIFY(first != 0);
        QCOMPARE(surface.nativeHandle(), first);
        QVERIFY(!surface.isWindow());
        QCOMPARE(surface.window(), &host);
    }

    void switchesStreamingStateWithoutCreatingTopLevelWindow() {
        QWidget host;
        VideoSurface surface(&host);
        surface.setStreaming(true);
        QVERIFY(surface.isStreaming());
        QVERIFY(!surface.isWindow());
        surface.setStreaming(false);
        QVERIFY(!surface.isStreaming());
    }
};

QTEST_MAIN(VideoSurfaceTest)
#include "test_videosurface.moc"
