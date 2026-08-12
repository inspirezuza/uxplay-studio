#include "export/exportjob.h"

#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

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
};

QTEST_APPLESS_MAIN(ExportJobTest)
#include "test_exportjob.moc"
