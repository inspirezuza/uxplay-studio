#pragma once

#include "studio/scenedocument.h"

#include <QDateTime>
#include <QList>
#include <QString>
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

class ProjectStore final {
public:
    explicit ProjectStore(QString rootDirectory);

    QString rootDirectory() const;
    ProjectCreateResult create(const SceneDocument &document);
    ProjectLoadResult load(const QString &directory) const;
    QString save(const ProjectInfo &project, const SceneDocument &document) const;
    QString setState(const QString &directory, ProjectState state);
    QList<ProjectSummary> projects() const;
    QList<ProjectSummary> recoverableProjects();

private:
    QString writeManifest(const ProjectInfo &project, const SceneDocument &document) const;
    QString m_rootDirectory;
};

QString projectStateKey(ProjectState state);

