#include "videosurface.h"
#include "renderers/video_renderer.h"

#include <QtTest>

class VideoSurfaceTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void previewBranchBuildsWithoutChangingWindowOwnership() {
        logger_t *logger = logger_init();
        QVERIFY(logger);
        videoflip_t transforms[2] = {NONE, NONE};
        video_renderer_set_window_handle(0);
        QVERIFY(video_renderer_init(logger, "UxPlay Studio test", transforms, "h264parse", "",
                                    "decodebin", "videoconvert", "fakesink", "", false,
                                    false, false, false, 3, nullptr));
        video_renderer_destroy();
        logger_destroy(logger);
    }

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
