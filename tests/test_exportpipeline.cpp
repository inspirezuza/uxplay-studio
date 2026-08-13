#include "export/exportpipeline.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

class ExportPipelineTest final : public QObject {
    Q_OBJECT
private slots:
    void timelineDurationIncludesMeasuredTrackStartOffsets() {
        const QHash<QString, qint64> durations {
            {QStringLiteral("airplay-video"), 2'000'000'000LL},
            {QStringLiteral("camera"), 1'500'000'000LL},
            {QStringLiteral("microphone"), 1'800'000'000LL}
        };
        const QHash<QString, qint64> offsets {
            {QStringLiteral("airplay-video"), 0},
            {QStringLiteral("camera"), 900'000'000LL},
            {QStringLiteral("microphone"), 100'000'000LL}
        };
        QCOMPARE(ExportPipeline::alignedTimelineDuration(durations, offsets),
                 2'400'000'000LL);
    }

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
        const QHash<QString, QSize> sourcePixelSizes{{airplay, QSize(1280, 720)},
                                                     {camera, QSize(640, 480)}};
        const auto built = ExportPipeline::build(project, scene, SceneFormat::Vertical,
                                                  temp.path() + "/out.mp4", 0,
                                                  sourcePixelSizes);
        QVERIFY2(built.ok(), qPrintable(built.error));
        QVERIFY(built.description.contains("width=1080,height=1920"));
        QVERIFY(built.description.contains(
            "videocrop left=128 right=128 top=36 bottom=36 ! videoscale"));
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

    void cropsSourcePixelsBeforeScalingAndKeepsRotatedCanvasGeometry() {
        QTemporaryDir temp;
        SceneDocument scene;
        const QString airplay = scene.addSource(SceneSourceType::AirPlay, QStringLiteral("iPad"));
        const QString layer = scene.addLayer(SceneFormat::Wide, airplay);
        SceneTransform transform;
        transform.frame = QRectF(50, 100, 800, 600);
        transform.crop = QMarginsF(.25, .10, .125, .20);
        transform.rotationDegrees = 30.0;
        transform.mask = SceneMask::RoundedRectangle;
        QVERIFY(scene.setTransform(SceneFormat::Wide, layer, transform));

        ProjectInfo project;
        project.directory = temp.path();
        QDir().mkpath(project.airplayDirectory());
        QFile track(QDir(project.airplayDirectory()).filePath(QStringLiteral("video-00000.mkv")));
        QVERIFY(track.open(QIODevice::WriteOnly));
        track.close();

        const auto built = ExportPipeline::build(
            project, scene, SceneFormat::Wide, temp.path() + QStringLiteral("/crop.mp4"), 0,
            {{airplay, QSize(640, 360)}});
        QVERIFY2(built.ok(), qPrintable(built.error));
        const QString sourceCrop = QStringLiteral(
            "videocrop left=160 right=80 top=36 bottom=72");
        const QString destinationScale = QStringLiteral(
            "videoscale ! video/x-raw,width=500,height=420");
        const QString mask = QStringLiteral("cairooverlay name=rounded-mask-0");
        const QString paddedRotation = QStringLiteral(
            "videobox left=-297 right=-197 top=-220 bottom=-280 border-alpha=0 "
            "! video/x-raw,format=BGRA,width=994,height=920 ! rotate angle=0.52359878");
        const qsizetype cropIndex = built.description.indexOf(sourceCrop);
        const qsizetype scaleIndex = built.description.indexOf(destinationScale);
        const qsizetype maskIndex = built.description.indexOf(mask);
        const qsizetype rotationIndex = built.description.indexOf(paddedRotation);
        QVERIFY(cropIndex >= 0);
        QVERIFY(scaleIndex > cropIndex);
        QVERIFY(maskIndex > scaleIndex);
        QVERIFY(rotationIndex > maskIndex);
        QVERIFY(built.description.contains(QStringLiteral("sink_0::xpos=-47")));
        QVERIFY(built.description.contains(QStringLiteral("sink_0::ypos=-60")));

        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(built.description.toUtf8().constData(), &error);
        QVERIFY2(pipeline && !error, error ? error->message : "Crop pipeline did not parse");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
    }

    void alignsIndependentTracksToTheAirplayTimeline() {
        QTemporaryDir temp;
        SceneDocument scene;
        const QString airplay = scene.addSource(SceneSourceType::AirPlay, QStringLiteral("iPad"));
        const QString camera = scene.addSource(SceneSourceType::Camera, QStringLiteral("Presenter"));
        scene.addLayer(SceneFormat::Wide, airplay);
        scene.addLayer(SceneFormat::Wide, camera);

        ProjectInfo project;
        project.directory = temp.path();
        QDir().mkpath(project.airplayDirectory());
        QDir().mkpath(project.presenterDirectory());
        for (const QString &path : {
                 QDir(project.airplayDirectory()).filePath(QStringLiteral("video-00000.mkv")),
                 QDir(project.airplayDirectory()).filePath(QStringLiteral("audio-00000.mka")),
                 QDir(project.presenterDirectory()).filePath(QStringLiteral("camera-00000.mkv")),
                 QDir(project.presenterDirectory()).filePath(QStringLiteral("microphone-00000.mka"))}) {
            QFile track(path);
            QVERIFY(track.open(QIODevice::WriteOnly));
        }
        QFile manifest(QDir(project.directory).filePath(QStringLiteral("session.json")));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        const QJsonObject offsets{{QStringLiteral("airplay-video"), 0},
                                  {QStringLiteral("airplay-audio"), 25'000'000},
                                  {QStringLiteral("camera"), 125'000'000},
                                  {QStringLiteral("microphone"), 275'000'000}};
        manifest.write(QJsonDocument(QJsonObject{
            {QStringLiteral("trackStartOffsetsNanoseconds"), offsets}}).toJson());
        manifest.close();

        const auto built = ExportPipeline::build(project, scene, SceneFormat::Wide,
                                                  temp.path() + QStringLiteral("/aligned.mp4"));
        QVERIFY2(built.ok(), qPrintable(built.error));
        QVERIFY(built.description.contains(QStringLiteral(
            "video-00000.mkv\" ! decodebin ! identity ts-offset=0")));
        QVERIFY(built.description.contains(QStringLiteral(
            "camera-00000.mkv\" ! decodebin ! identity ts-offset=125000000")));
        QVERIFY(built.description.contains(QStringLiteral(
            "audio-*.mka\" ! decodebin ! identity ts-offset=25000000")));
        QVERIFY(built.description.contains(QStringLiteral(
            "microphone-*.mka\" ! decodebin ! identity ts-offset=275000000")));

        GError *error = nullptr;
        GstElement *pipeline = gst_parse_launch(built.description.toUtf8().constData(), &error);
        QVERIFY2(pipeline && !error, error ? error->message : "Aligned pipeline did not parse");
        if (error) g_error_free(error);
        if (pipeline) gst_object_unref(pipeline);
    }
};

QTEST_APPLESS_MAIN(ExportPipelineTest)
#include "test_exportpipeline.moc"
