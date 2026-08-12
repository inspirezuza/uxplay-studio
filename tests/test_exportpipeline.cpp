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
        t.crop = QMarginsF(.1, .05, .1, .05);
        QVERIFY(scene.setTransform(SceneFormat::Vertical, airplayLayer, t));
        t.frame = QRectF(700, 1510, 320, 320);
        t.crop = {};
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
        QVERIFY(built.description.contains("video/x-raw,width=720,height=1260"));
        QVERIFY(built.description.contains("sink_0::xpos=140"));
        QVERIFY(built.description.contains("sink_0::alpha=0.8"));
        QVERIFY(built.description.contains("sink_1::xpos=700"));
        QVERIFY(built.description.contains("mp4mux"));
        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(built.description.toUtf8().constData(), &error);
        QVERIFY2(pipeline && !error, error ? error->message : "Export pipeline did not parse");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
    }

    void boundsStaticSourcesAtTheTimelineDuration() {
        QTemporaryDir temp;
        SceneDocument scene;
        const QString color = scene.addSource(SceneSourceType::Color, QStringLiteral("Backdrop"),
                                              QStringLiteral("#123456"));
        scene.addLayer(SceneFormat::Wide, color);
        ProjectInfo project;
        project.directory = temp.path();

        const auto built = ExportPipeline::build(project, scene, SceneFormat::Wide,
                                                  temp.path() + QStringLiteral("/static.mp4"),
                                                  2'000'000'000);
        QVERIFY2(built.ok(), qPrintable(built.error));
        QVERIFY(built.description.contains(QStringLiteral("foreground-color=4279383126")));
        QVERIFY(built.description.contains(QStringLiteral("num-buffers=120")));
        QVERIFY(built.description.contains(QStringLiteral("framerate=60/1")));
    }
};

QTEST_APPLESS_MAIN(ExportPipelineTest)
#include "test_exportpipeline.moc"
