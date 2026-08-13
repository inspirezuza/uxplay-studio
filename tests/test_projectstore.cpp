#include "projects/projectstore.h"

#include <QFile>
#include <QThread>
#include <QTemporaryDir>
#include <QtTest>
#include <gst/gst.h>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <atomic>

namespace {
bool writePlayableVideoFragment(const QString &path, QString *error) {
    QString escapedPath = QDir::fromNativeSeparators(path);
    escapedPath.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    const QString description = QStringLiteral(
        "videotestsrc num-buffers=6 ! video/x-raw,width=160,height=90,framerate=30/1 "
        "! videoconvert ! openh264enc bitrate=200000 ! h264parse ! matroskamux "
        "! filesink location=\"%1\"").arg(escapedPath);
    GError *parseError = nullptr;
    GstElement *pipeline = gst_parse_launch(description.toUtf8().constData(), &parseError);
    if (!pipeline || parseError) {
        if (error) *error = parseError ? QString::fromUtf8(parseError->message)
                                      : QStringLiteral("Could not create media test pipeline");
        if (parseError) g_error_free(parseError);
        if (pipeline) gst_object_unref(pipeline);
        return false;
    }
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstMessage *message = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (!ok && error) *error = QStringLiteral("Media test pipeline did not finish");
    if (message) gst_message_unref(message);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(pipeline);
    return ok;
}
}

class ProjectStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { gst_init(nullptr, nullptr); }

    void createsAndLoadsAnEditableLocalProject() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ProjectStore store(root.path());

        SceneDocument document;
        document.setTitle(QStringLiteral("Calculus review"));
        const QString source = document.addSource(SceneSourceType::AirPlay,
                                                  QStringLiteral("Faith's iPad"));
        document.addLayer(SceneFormat::Wide, source);

        const auto created = store.create(document);
        QVERIFY2(created.ok(), qPrintable(created.error));
        QVERIFY(QFile::exists(created.project.manifestPath()));
        QVERIFY(QFile::exists(created.project.airplayDirectory()));
        QVERIFY(QFile::exists(created.project.presenterDirectory()));
        QVERIFY(QFile::exists(created.project.exportsDirectory()));

        const auto loaded = store.load(created.project.directory);
        QVERIFY2(loaded.ok(), qPrintable(loaded.error));
        QCOMPARE(loaded.project.title, QStringLiteral("Calculus review"));
        QCOMPARE(loaded.project.state, ProjectState::Ready);
        QCOMPARE(loaded.document->composition(SceneFormat::Wide).layers.size(), 1);
    }

    void findsInterruptedSessionsWithoutDestroyingTheirMedia() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        ProjectStore store(root.path());
        SceneDocument document;
        document.setTitle(QStringLiteral("Interrupted lesson"));

        auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());

        QFile fragment(created.project.airplayDirectory() + QStringLiteral("/segment-00001.mkv"));
        QVERIFY(fragment.open(QIODevice::WriteOnly));
        fragment.write("finalized fragment");
        fragment.close();

        const QList<ProjectSummary> recoverable = store.recoverableProjects();
        QCOMPARE(recoverable.size(), 1);
        QCOMPARE(recoverable.first().title, QStringLiteral("Interrupted lesson"));
        QCOMPARE(recoverable.first().state, ProjectState::Recoverable);
        QVERIFY(QFile::exists(fragment.fileName()));
    }

    void saveDoesNotOverwriteAConcurrentRecordingState() {
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());

        document.setTitle(QStringLiteral("Edited while recording"));
        QVERIFY(store.save(created.project, document).isEmpty());
        const auto loaded = store.load(created.project.directory);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.project.state, ProjectState::Recording);
        QCOMPARE(loaded.document->title(), QStringLiteral("Edited while recording"));
    }

    void recoveryPreservesUsableMediaAndQuarantinesIncompleteFragments() {
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());
        QCOMPARE(store.recoverableProjects().size(), 1);

        const QString usable = QDir(created.project.airplayDirectory())
            .filePath(QStringLiteral("video-00000.mkv"));
        const QString incomplete = QDir(created.project.airplayDirectory())
            .filePath(QStringLiteral("video-00001.mkv"));
        QString mediaError;
        QVERIFY2(writePlayableVideoFragment(usable, &mediaError), qPrintable(mediaError));
        QFile broken(incomplete);
        QVERIFY(broken.open(QIODevice::WriteOnly));
        QVERIFY(broken.write("not a Matroska fragment") > 0);
        broken.close();

        const ProjectRecoveryResult recovered = store.recover(created.project.directory);
        QVERIFY2(recovered.ok(), qPrintable(recovered.error));
        QCOMPARE(recovered.usableMediaFiles, 1);
        QCOMPARE(recovered.quarantinedMediaFiles, 1);
        QVERIFY(QFileInfo::exists(usable));
        QVERIFY(!QFileInfo::exists(incomplete));
        QVERIFY(QFileInfo::exists(incomplete + QStringLiteral(".incomplete")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
    }

    void recoveryFailsClosedWithoutUsableAirplayVideo() {
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Recording).isEmpty());
        store.recoverableProjects();

        const ProjectRecoveryResult recovered = store.recover(created.project.directory);
        QVERIFY(!recovered.ok());
        QVERIFY(recovered.error.contains(QStringLiteral("No usable AirPlay video")));
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Recoverable);
    }

    void interruptedExportRestoresReadyWithoutMediaRecovery() {
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Exporting).isEmpty());
        const QString partial = QDir(created.project.exportsDirectory())
            .filePath(QStringLiteral("interrupted.mp4.partial"));
        QFile partialFile(partial);
        QVERIFY(partialFile.open(QIODevice::WriteOnly));
        QVERIFY(partialFile.write("unfinished") > 0);
        partialFile.close();

        QVERIFY(store.recoverableProjects().isEmpty());
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
        QVERIFY(!QFileInfo::exists(partial));
    }

    void interruptedExportStaysExportingWhenPartialCleanupIsBlocked() {
#ifndef Q_OS_WIN
        QSKIP("The deterministic file-lock seam is Windows-specific");
#else
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());
        QVERIFY(store.setState(created.project.directory, ProjectState::Exporting).isEmpty());
        const QString partial = QDir(created.project.exportsDirectory())
            .filePath(QStringLiteral("locked.mp4.partial"));
        const HANDLE lock = CreateFileW(
            reinterpret_cast<LPCWSTR>(partial.utf16()), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);

        QVERIFY(store.recoverableProjects().isEmpty());
        QCOMPARE(store.load(created.project.directory).project.state,
                 ProjectState::Exporting);
        CloseHandle(lock);

        QVERIFY(store.recoverableProjects().isEmpty());
        QCOMPARE(store.load(created.project.directory).project.state, ProjectState::Ready);
        QVERIFY(!QFileInfo::exists(partial));
#endif
    }

    void concurrentStateAndSceneUpdatesPreserveBoth() {
        QTemporaryDir root;
        ProjectStore store(root.path());
        SceneDocument document;
        const auto created = store.create(document);
        QVERIFY(created.ok());

        SceneDocument edited;
        edited.setTitle(QStringLiteral("Concurrent canvas edit"));
        std::atomic_bool stateOk{true};
        std::atomic_bool sceneOk{true};
        QThread *stateThread = QThread::create([&]() {
            for (int i = 0; i < 100; ++i)
                if (!store.setState(created.project.directory, ProjectState::Exporting).isEmpty())
                    stateOk = false;
        });
        QThread *sceneThread = QThread::create([&]() {
            for (int i = 0; i < 100; ++i)
                if (!store.save(created.project, edited).isEmpty()) sceneOk = false;
        });
        stateThread->start();
        sceneThread->start();
        QVERIFY(stateThread->wait(10000));
        QVERIFY(sceneThread->wait(10000));
        delete stateThread;
        delete sceneThread;
        QVERIFY(stateOk.load());
        QVERIFY(sceneOk.load());

        const auto loaded = store.load(created.project.directory);
        QVERIFY(loaded.ok());
        QCOMPARE(loaded.project.state, ProjectState::Exporting);
        QCOMPARE(loaded.document->title(), QStringLiteral("Concurrent canvas edit"));
    }
};

QTEST_GUILESS_MAIN(ProjectStoreTest)
#include "test_projectstore.moc"
