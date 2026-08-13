#include "recording/camerapreviewengine.h"
#include "studio/cameraselfview.h"

#include <QSignalSpy>
#include <QWidget>
#include <QtTest>

class CameraPreviewEngineTest final : public QObject {
    Q_OBJECT

private slots:
    void streamsBoundedPreviewFramesWithoutAWindow() {
        CameraPreviewEngine preview;
        QSignalSpy frames(&preview, &CameraPreviewEngine::frameReady);
        QString error;
        QVERIFY2(preview.start(&error, QStringLiteral("videotestsrc is-live=true pattern=ball")),
                 qPrintable(error));
        QTRY_VERIFY_WITH_TIMEOUT(!frames.isEmpty(), 5000);
        const QImage frame = qvariant_cast<QImage>(frames.first().first());
        QCOMPARE(frame.size(), QSize(640, 360));
        preview.stop();
        QVERIFY(!preview.isRunning());
    }

    void reportsUnexpectedEndOfStreamAndStopsThePreview() {
        CameraPreviewEngine preview;
        QSignalSpy stopped(&preview, &CameraPreviewEngine::stoppedUnexpectedly);
        QString error;
        QVERIFY2(preview.start(
                     &error,
                     QStringLiteral("videotestsrc is-live=true ! identity error-after=12")),
                 qPrintable(error));
        QTRY_COMPARE_WITH_TIMEOUT(stopped.count(), 1, 5000);
        QVERIFY(!preview.isRunning());
        QVERIFY(!stopped.first().first().toString().isEmpty());
    }

    void selfViewRemainsAChildInsideTheStudioWindow() {
        QWidget studio;
        studio.resize(900, 600);
        CameraSelfView selfView(&studio);
        selfView.setFrame(QImage(640, 360, QImage::Format_ARGB32));
        selfView.setActive(true);

        QVERIFY(selfView.isVisibleTo(&studio));
        QVERIFY(!selfView.isWindow());
        QCOMPARE(selfView.parentWidget(), &studio);
        QVERIFY(studio.rect().contains(selfView.geometry()));
    }
};

QTEST_MAIN(CameraPreviewEngineTest)
#include "test_camerapreview.moc"
