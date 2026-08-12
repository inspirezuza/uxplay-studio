#include "projects/projectstore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class ProjectStoreTest final : public QObject {
    Q_OBJECT

private slots:
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
};

QTEST_GUILESS_MAIN(ProjectStoreTest)
#include "test_projectstore.moc"
