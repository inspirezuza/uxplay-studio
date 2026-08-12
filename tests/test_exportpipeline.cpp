#include "export/exportpipeline.h"

#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

class ExportPipelineTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void translatesSceneLayersIntoACompositedMp4Pipeline() {
        QTemporaryDir temp;
        SceneDocument scene;
        const QString airplay = scene.addSource(SceneSourceType::AirPlay, "iPad");
        const QString camera = scene.addSource(SceneSourceType::Camera, "Presenter");
        const QString airplayLayer = scene.addLayer(SceneFormat::Vertical, airplay);
        const QString cameraLayer = scene.addLayer(SceneFormat::Vertical, camera);
        SceneTransform t;
        t.frame = QRectF(50, 100, 900, 1400);
        t.opacity = .8;
        QVERIFY(scene.setTransform(SceneFormat::Vertical, airplayLayer, t));
        t.frame = QRectF(700, 1510, 320, 320);
        t.mask = SceneMask::Circle;
        QVERIFY(scene.setTransform(SceneFormat::Vertical, cameraLayer, t));

        ProjectInfo project;
        project.directory = temp.path();
        QDir().mkpath(project.airplayDirectory());
        QDir().mkpath(project.presenterDirectory());
        QFile airplayTrack(project.airplayDirectory() + "/video-00000.mkv");
        QFile cameraTrack(project.presenterDirectory() + "/camera-00000.mkv");
        QVERIFY(airplayTrack.open(QIODevice::WriteOnly));
        QVERIFY(cameraTrack.open(QIODevice::WriteOnly));
        airplayTrack.close();
        cameraTrack.close();
        const auto built = ExportPipeline::build(project, scene, SceneFormat::Vertical,
                                                  temp.path() + "/out.mp4");
        QVERIFY2(built.ok(), qPrintable(built.error));
        QVERIFY(built.description.contains("width=1080,height=1920"));
        QVERIFY(built.description.contains("sink_0::xpos=50"));
        QVERIFY(built.description.contains("sink_0::alpha=0.8"));
        QVERIFY(built.description.contains("sink_1::xpos=700"));
        QVERIFY(built.description.contains("mp4mux"));
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(built.description.toUtf8().constData(), &error);
        QVERIFY2(pipeline && !error, error ? error->message : "Export pipeline did not parse");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
    }
};

QTEST_APPLESS_MAIN(ExportPipelineTest)
#include "test_exportpipeline.moc"
