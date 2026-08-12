#include "projects/projectstore.h"
#include "recording/recordingsession.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>

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
    void initTestCase() { gst_init(nullptr, nullptr); }

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
        QCOMPARE(runner.names, QStringList({QStringLiteral("airplay-video"),
                                           QStringLiteral("airplay-audio"),
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
        QCOMPARE(runner.updatedRect, QRect(20, 30, 960, 540));

        QVERIFY(session.stop());
        QVERIFY(runner.stopped);
        QCOMPARE(session.state(), RecordingState::Idle);
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
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
        runner.failed = {QStringLiteral("airplay-video")};
        QVERIFY(!session.start(second.project, options));
        QCOMPARE(store.load(second.project.directory).project.state, ProjectState::Failed);
    }
};

QTEST_APPLESS_MAIN(RecordingSessionTest)
#include "test_recordingsession.moc"
