#include "export/exportjob.h"

#include <QFileInfo>
#include <QImage>
#include <QSignalSpy>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>

namespace {
bool runPipeline(const QString &description, QString *error) {
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!pipeline || parseError) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message) : QStringLiteral("parse failed");
        if (parseError) g_error_free(parseError);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 15 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (!ok && error) *error = QStringLiteral("pipeline did not reach EOS");
    if (message) gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ok;
}

QImage decodeFirstVideoFrame(const QString &path, QString *error) {
    const QString description = QStringLiteral(
        "uridecodebin uri=\"%1\" ! videoconvert ! video/x-raw,format=RGB "
        "! appsink name=frame-sink max-buffers=1 drop=true sync=false")
        .arg(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!pipeline || parseError) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message)
                                      : QStringLiteral("frame decoder did not parse");
        if (parseError) g_error_free(parseError);
        if (pipeline) gst_object_unref(pipeline);
        return {};
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "frame-sink");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 15 * GST_SECOND);
    QImage image;
    if (sample) {
        GstVideoInfo info;
        GstVideoFrame frame;
        if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
            gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) {
            const auto *pixels = static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0));
            image = QImage(pixels, GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame),
                           GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), QImage::Format_RGB888).copy();
            gst_video_frame_unmap(&frame);
        }
        gst_sample_unref(sample);
    }
    if (image.isNull() && error) *error = QStringLiteral("could not decode an exported video frame");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (sink) gst_object_unref(sink);
    gst_object_unref(pipeline);
    return image;
}
}

class ExportJobTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void rendersARealMaskedCompositionToMp4() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        scene.setTitle(QStringLiteral("Export smoke test"));
        const QString airplay = scene.addSource(SceneSourceType::AirPlay, QStringLiteral("iPad"));
        const QString camera = scene.addSource(SceneSourceType::Camera, QStringLiteral("Presenter"));
        const QString title = scene.addSource(SceneSourceType::Text, QStringLiteral("Lecture title"));
        scene.addLayer(SceneFormat::Wide, airplay);
        const QString cameraLayer = scene.addLayer(SceneFormat::Wide, camera);
        scene.addLayer(SceneFormat::Wide, title);
        SceneTransform transform = scene.layer(SceneFormat::Wide, cameraLayer)->transform;
        transform.frame = QRectF(1400, 700, 360, 300);
        transform.crop = QMarginsF(.05, .05, .05, .05);
        transform.rotationDegrees = 7.0;
        transform.mask = SceneMask::RoundedRectangle;
        QVERIFY(scene.setTransform(SceneFormat::Wide, cameraLayer, transform));
        const auto created = store.create(scene);
        QVERIFY(created.ok());

        QString error;
        const QString videoTemplate = QStringLiteral(
            "videotestsrc num-buffers=18 pattern=%1 ! video/x-raw,width=640,height=360,framerate=30/1 "
            "! videoconvert ! openh264enc bitrate=1000000 ! h264parse ! matroskamux ! filesink location=\"%2\"");
        QVERIFY2(runPipeline(videoTemplate.arg(QStringLiteral("smpte"),
            QDir(created.project.airplayDirectory()).filePath(QStringLiteral("video-00000.mkv"))), &error), qPrintable(error));
        QVERIFY2(runPipeline(videoTemplate.arg(QStringLiteral("ball"),
            QDir(created.project.presenterDirectory()).filePath(QStringLiteral("camera-00000.mkv"))), &error), qPrintable(error));

        ExportJob job(&store);
        QSignalSpy finished(&job, &ExportJob::finished);
        QSignalSpy failed(&job, &ExportJob::failed);
        const QString output = QDir(created.project.exportsDirectory()).filePath(QStringLiteral("wide.mp4"));
        QVERIFY(job.start(created.project, scene, SceneFormat::Wide, output));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1 || failed.count() == 1, 35000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
        QVERIFY(QFileInfo(output).size() > 1024);
    }

    void exportsTheCroppedSourceRectangleInsteadOfSquashingTheFullImage() {
        QTemporaryDir temp;
        const QString imagePath = QDir(temp.path()).filePath(QStringLiteral("crop-source.png"));
        QImage sourceImage(640, 360, QImage::Format_RGB32);
        for (int y = 0; y < sourceImage.height(); ++y) {
            QRgb *row = reinterpret_cast<QRgb *>(sourceImage.scanLine(y));
            for (int x = 0; x < sourceImage.width(); ++x) {
                row[x] = x < 160 ? qRgb(255, 0, 0)
                                 : x < 480 ? qRgb(0, 255, 0) : qRgb(0, 0, 255);
            }
        }
        QVERIFY(sourceImage.save(imagePath));

        ProjectStore store(temp.path());
        SceneDocument scene;
        const QString image = scene.addSource(SceneSourceType::Image,
                                              QStringLiteral("Crop reference"), imagePath);
        const QString layer = scene.addLayer(SceneFormat::Wide, image);
        SceneTransform transform;
        transform.frame = QRectF(100, 100, 640, 360);
        transform.crop = QMarginsF(.25, 0, .125, 0);
        QVERIFY(scene.setTransform(SceneFormat::Wide, layer, transform));
        const auto created = store.create(scene);
        QVERIFY(created.ok());

        QString error;
        const QString timeline = QStringLiteral(
            "videotestsrc num-buffers=12 pattern=black "
            "! video/x-raw,width=640,height=360,framerate=30/1 "
            "! videoconvert ! openh264enc bitrate=1000000 ! h264parse ! matroskamux "
            "! filesink location=\"%1\"")
            .arg(QDir(created.project.airplayDirectory()).filePath(
                QStringLiteral("video-00000.mkv")));
        QVERIFY2(runPipeline(timeline, &error), qPrintable(error));

        ExportJob job(&store);
        QSignalSpy finished(&job, &ExportJob::finished);
        QSignalSpy failed(&job, &ExportJob::failed);
        const QString output = QDir(created.project.exportsDirectory())
            .filePath(QStringLiteral("cropped-source.mp4"));
        QVERIFY(job.start(created.project, scene, SceneFormat::Wide, output));
        QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1 || failed.count() == 1, 35000);
        QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));

        const QImage frame = decodeFirstVideoFrame(output, &error);
        QVERIFY2(!frame.isNull(), qPrintable(error));
        QCOMPARE(frame.size(), QSize(1920, 1080));
        const QColor outside = frame.pixelColor(150, 200);
        const QColor croppedLeft = frame.pixelColor(280, 200);
        const QColor croppedRight = frame.pixelColor(620, 200);
        QVERIFY2(outside.red() < 40 && outside.green() < 40 && outside.blue() < 40,
                 qPrintable(outside.name()));
        QVERIFY2(croppedLeft.green() > croppedLeft.red() + 80 &&
                     croppedLeft.green() > croppedLeft.blue() + 80,
                 qPrintable(croppedLeft.name()));
        QVERIFY2(croppedRight.blue() > croppedRight.red() + 80 &&
                     croppedRight.blue() > croppedRight.green() + 80,
                 qPrintable(croppedRight.name()));
    }

    void destructionCancelsDiscoveryBeforeStartingAnExport() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        scene.addSource(SceneSourceType::AirPlay, QStringLiteral("iPad"));
        const auto created = store.create(scene);
        QVERIFY(created.ok());
        for (int index = 0; index < 100; ++index) {
            QFile corrupt(QDir(created.project.airplayDirectory())
                              .filePath(QStringLiteral("video-%1.mkv").arg(index, 5, 10, QLatin1Char('0'))));
            QVERIFY(corrupt.open(QIODevice::WriteOnly));
            corrupt.write("not-media");
        }

        const QString output = QDir(created.project.exportsDirectory()).filePath(QStringLiteral("cancelled.mp4"));
        QElapsedTimer elapsed;
        elapsed.start();
        {
            ExportJob job(&store);
            QVERIFY(job.start(created.project, scene, SceneFormat::Wide, output));
            QTest::qWait(20);
        }
        QVERIFY2(elapsed.elapsed() < 5000, "Export cancellation scanned every segment");
        QVERIFY(!QFileInfo::exists(output));
        QVERIFY(!QFileInfo::exists(output + QStringLiteral(".partial")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
    }

    void failedExportKeepsTheSourceProjectReadyForRetry() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        const QString airplay = scene.addSource(SceneSourceType::AirPlay,
                                                QStringLiteral("Missing recording"));
        scene.addLayer(SceneFormat::Wide, airplay);
        const auto created = store.create(scene);
        QVERIFY(created.ok());

        ExportJob job(&store);
        QSignalSpy failed(&job, &ExportJob::failed);
        const QString output = QDir(created.project.exportsDirectory())
            .filePath(QStringLiteral("failed.mp4"));
        QVERIFY(job.start(created.project, scene, SceneFormat::Wide, output));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 10000);
        QVERIFY(!QFileInfo::exists(output));
        QVERIFY(!QFileInfo::exists(output + QStringLiteral(".partial")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);

        QTRY_VERIFY(!job.isRunning());
        QVERIFY(job.start(created.project, scene, SceneFormat::Wide, output));
        QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 2, 10000);
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
    }

    void recoverableProjectIsRejectedWithoutChangingItsState() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        const QString airplay = scene.addSource(SceneSourceType::AirPlay,
                                                QStringLiteral("Interrupted recording"));
        scene.addLayer(SceneFormat::Wide, airplay);
        const auto created = store.create(scene);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recoverable).isEmpty());

        ExportJob job(&store);
        QSignalSpy failed(&job, &ExportJob::failed);
        const QString output = QDir(created.project.exportsDirectory())
            .filePath(QStringLiteral("must-not-export.mp4"));
        QVERIFY(!job.start(created.project, scene, SceneFormat::Wide, output));
        QCOMPARE(failed.count(), 1);
        QVERIFY(failed.first().first().toString().contains(QStringLiteral("Recover")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recoverable);
        QVERIFY(!QFileInfo::exists(output));
    }
};

QTEST_APPLESS_MAIN(ExportJobTest)
#include "test_exportjob.moc"
