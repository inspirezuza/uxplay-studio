#pragma once

#include "studio/scenedocument.h"

#include <QDateTime>
#include <QList>
#include <QMutex>
#include <QString>
#include <functional>
#include <memory>

enum class ProjectState {
    Ready,
    Recording,
    Finalizing,
    Recoverable,
    Exporting,
    Failed
};

struct ProjectInfo {
    QString id;
    QString title;
    QString directory;
    ProjectState state = ProjectState::Ready;
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;

    QString manifestPath() const;
    QString airplayDirectory() const;
    QString presenterDirectory() const;
    QString exportsDirectory() const;
};

struct ProjectSummary {
    QString id;
    QString title;
    QString directory;
    ProjectState state = ProjectState::Ready;
    QDateTime updatedAtUtc;
};

struct ProjectCreateResult {
    ProjectInfo project;
    QString error;
    bool ok() const { return error.isEmpty() && !project.directory.isEmpty(); }
};

struct ProjectLoadResult {
    ProjectInfo project;
    std::unique_ptr<SceneDocument> document;
    QString error;
    bool ok() const { return error.isEmpty() && document != nullptr; }
};

struct ProjectRecoveryResult {
    int usableMediaFiles = 0;
    int quarantinedMediaFiles = 0;
    QString error;
    bool ok() const { return error.isEmpty(); }
};

class ProjectStore final {
public:
    explicit ProjectStore(QString rootDirectory);

    QString rootDirectory() const;
    ProjectCreateResult create(const SceneDocument &document);
    ProjectLoadResult load(const QString &directory) const;
    QString save(const ProjectInfo &project, const SceneDocument &document) const;
    QString setState(const QString &directory, ProjectState state);
    ProjectRecoveryResult recover(const QString &directory,
                                  const std::function<bool()> &cancelled = {});
    QList<ProjectSummary> projects() const;
    QList<ProjectSummary> recoverableProjects();

private:
    ProjectLoadResult loadUnlocked(const QString &directory) const;
    QString writeManifest(const ProjectInfo &project, const SceneDocument &document) const;
    QString m_rootDirectory;
    mutable QMutex m_manifestMutex;
};

QString projectStateKey(ProjectState state);
