#include "projects/projectstore.h"
#include "recording/recordingsession.h"
#include "uxplay_api.h"
#include "renderers/mux_renderer.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace {
void discardLog(void *, int, const char *) {}
}

class FakePipelineRunner final : public PipelineRunner {
public:
    bool startTrack(const QString &name, const QString &pipeline, QString *error) override {
        Q_UNUSED(error)
        names.append(name);
        pipelines.append(pipeline);
        return !failed.contains(name);
    }
    bool stopAll(int, QString *) override { stopped = true; return true; }
    bool updateVideoCapture(const QRect &captureRect, QString *) override {
        updatedRect = captureRect;
        return true;
    }
    QStringList names;
    QStringList pipelines;
    QStringList failed;
    bool stopped = false;
    QRect updatedRect;
};

class RecordingSessionTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);
        uxplay_set_recording_test_mode(1);
    }
    void cleanupTestCase() { uxplay_set_recording_test_mode(0); }

    void startsIndependentRecoverableTracksAndFinalizesProject() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        scene.setTitle(QStringLiteral("Lecture"));
        const auto created = store.create(scene);
        QVERIFY(created.ok());
        FakePipelineRunner runner;
        RecordingSession session(&store, &runner);
        RecordingOptions options;
        options.captureRect = QRect(10, 20, 1280, 720);
        options.camera = true;
        options.microphone = true;
        options.systemAudio = true;

        QVERIFY2(session.start(created.project, options), qPrintable(session.lastError()));
        QCOMPARE(session.state(), RecordingState::Recording);
        QCOMPARE(runner.names, QStringList({QStringLiteral("airplay-audio"),
                                           QStringLiteral("camera"),
                                           QStringLiteral("microphone")}));
        for (const QString &description : runner.pipelines)
            QVERIFY(description.contains(QStringLiteral("%05d")));
        for (const QString &description : runner.pipelines) {
            GError *error = nullptr;
            GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &error);
            const QByteArray failure = (error ? QByteArray(error->message) : QByteArray("Pipeline did not parse"))
                + QByteArray("\n") + description.toUtf8();
            QVERIFY2(pipeline && !error, failure.constData());
            if (error) g_error_free(error);
            if (pipeline) gst_object_unref(pipeline);
        }
        QVERIFY(QFile::exists(QDir(created.project.directory).filePath("session.json")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recording);
        QVERIFY(session.updateCaptureRect(0, QRect(20, 30, 960, 540)));

        QVERIFY(session.stop());
        QVERIFY(runner.stopped);
        QCOMPARE(session.state(), RecordingState::Idle);
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
    }

    void directAirplayMuxRejectsEmptyFinalization() {
        QTemporaryDir temp;
        QVERIFY(QDir(temp.path()).mkdir(QStringLiteral("space in path")));
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("space in path/video")).toUtf8();
        logger_t *logger = logger_init();
        QVERIFY(logger);
        logger_set_callback(logger, discardLog, nullptr);
        mux_renderer_init(logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));
        QVERIFY(!mux_renderer_stop());
        mux_renderer_destroy();
        logger_destroy(logger);
    }

    void directAirplayMuxProducesARecoverableVideoTrack() {
        QTemporaryDir temp;
        QVERIFY(QDir(temp.path()).mkdir(QStringLiteral("space in path")));
        const QDir outputDir(QDir(temp.path()).filePath(QStringLiteral("space in path")));
        const QByteArray prefix = outputDir.filePath(QStringLiteral("video")).toUtf8();
        logger_t *logger = logger_init();
        QVERIFY(logger);
        logger_set_callback(logger, discardLog, nullptr);

        GError *error = nullptr;
        GstElement *source = gst_parse_launch(
            "videotestsrc num-buffers=24 ! video/x-raw,width=320,height=180,framerate=30/1 "
            "! videoconvert ! openh264enc gop-size=90 ! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=encoded sync=false", &error);
        QVERIFY2(source && !error, error ? error->message : "Could not create encoded test stream");
        if (error) g_error_free(error);
        GstElement *sink = gst_bin_get_by_name(GST_BIN(source), "encoded");
        gst_element_set_state(source, GST_STATE_PLAYING);
        int frame = 0;
        int delivered = 0;
        bool recording = false;
        while (GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND)) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                mux_renderer_cache_video(map.data, static_cast<int>(map.size), false);
                if (frame == 6) {
                    mux_renderer_init(logger, prefix.constData(), false, true);
                    QVERIFY(mux_renderer_choose_video_codec(false));
                    recording = true;
                }
                if (recording) {
                    mux_renderer_push_video(map.data, static_cast<int>(map.size),
                                            static_cast<uint64_t>(delivered++) * GST_SECOND / 30);
                }
                gst_buffer_unmap(buffer, &map);
            }
            ++frame;
            gst_sample_unref(sample);
            if (frame == 15) {
                QVERIFY(mux_renderer_stop());
                QVERIFY(mux_renderer_choose_video_codec(false));
            }
        }
        gst_element_set_state(source, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(source);
        mux_renderer_destroy();
        logger_destroy(logger);

        QCOMPARE(frame, 24);
        QCOMPARE(delivered, 18);
        QVERIFY(QFileInfo(outputDir.filePath(QStringLiteral("video-00000.mkv"))).size() > 1024);
        QVERIFY(QFileInfo(outputDir.filePath(QStringLiteral("video-00001.mkv"))).size() > 1024);

        const QString playback = QStringLiteral("splitmuxsrc location=\"%1/video-*.mkv\" ! decodebin ! fakesink")
            .arg(QDir::fromNativeSeparators(outputDir.path()));
        error = nullptr;
        GstElement *joined = gst_parse_launch(playback.toUtf8().constData(), &error);
        QVERIFY2(joined && !error, error ? error->message : "Could not join recovered fragments");
        if (error) g_error_free(error);
        GstBus *bus = gst_element_get_bus(joined);
        gst_element_set_state(joined, GST_STATE_PLAYING);
        GstMessage *message = gst_bus_timed_pop_filtered(
            bus, 10 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        QVERIFY(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS);
        if (message) gst_message_unref(message);
        gst_element_set_state(joined, GST_STATE_NULL);
        gst_object_unref(bus);
        gst_object_unref(joined);
    }

    void requiresTheAirplayVideoTrackButKeepsOptionalTrackWarnings() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        const auto created = store.create(scene);
        FakePipelineRunner runner;
        runner.failed = {QStringLiteral("camera")};
        RecordingSession session(&store, &runner);
        RecordingOptions options;
        options.captureRect = QRect(0, 0, 640, 480);
        options.camera = true;
        QVERIFY(session.start(created.project, options));
        QVERIFY(session.warnings().join(' ').contains(QStringLiteral("camera")));
        QVERIFY(session.stop());

        const auto second = store.create(scene);
        uxplay_set_recording_test_mode(0);
        QVERIFY(!session.start(second.project, options));
        QCOMPARE(store.load(second.project.directory).project.state, ProjectState::Failed);
        uxplay_set_recording_test_mode(1);
    }

    void failedAirplayFinalizationKeepsTheProjectRecoverable() {
        QTemporaryDir temp;
        ProjectStore store(temp.path());
        SceneDocument scene;
        const auto created = store.create(scene);
        FakePipelineRunner runner;
        RecordingSession session(&store, &runner);
        RecordingOptions options;
        options.captureRect = QRect(0, 0, 640, 480);
        QVERIFY(session.start(created.project, options));

        uxplay_set_recording_test_stop_result(0);
        QVERIFY(!session.stop());
        uxplay_set_recording_test_stop_result(1);
        QCOMPARE(session.state(), RecordingState::Failed);
        QVERIFY(session.lastError().contains(QStringLiteral("AirPlay video track")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recoverable);
    }
};

QTEST_APPLESS_MAIN(RecordingSessionTest)
#include "test_recordingsession.moc"
