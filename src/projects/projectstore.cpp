#include "projectstore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>
#include <algorithm>

namespace {

ProjectState projectStateFromKey(const QString &key) {
    if (key == QStringLiteral("recording")) return ProjectState::Recording;
    if (key == QStringLiteral("finalizing")) return ProjectState::Finalizing;
    if (key == QStringLiteral("recoverable")) return ProjectState::Recoverable;
    if (key == QStringLiteral("exporting")) return ProjectState::Exporting;
    if (key == QStringLiteral("failed")) return ProjectState::Failed;
    return ProjectState::Ready;
}

ProjectSummary summaryOf(const ProjectInfo &info) {
    return {info.id, info.title, info.directory, info.state, info.updatedAtUtc};
}

} // namespace

QString ProjectInfo::manifestPath() const {
    return QDir(directory).filePath(QStringLiteral("project.json"));
}
QString ProjectInfo::airplayDirectory() const {
    return QDir(directory).filePath(QStringLiteral("airplay"));
}
QString ProjectInfo::presenterDirectory() const {
    return QDir(directory).filePath(QStringLiteral("presenter"));
}
QString ProjectInfo::exportsDirectory() const {
    return QDir(directory).filePath(QStringLiteral("exports"));
}

ProjectStore::ProjectStore(QString rootDirectory)
    : m_rootDirectory(QDir::cleanPath(std::move(rootDirectory))) {}

QString ProjectStore::rootDirectory() const { return m_rootDirectory; }

ProjectCreateResult ProjectStore::create(const SceneDocument &document) {
    ProjectInfo info;
    info.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    info.title = document.title();
    info.directory = QDir(m_rootDirectory).filePath(info.id);
    info.state = ProjectState::Ready;
    info.createdAtUtc = QDateTime::currentDateTimeUtc();
    info.updatedAtUtc = info.createdAtUtc;

    QDir dir;
    if (!dir.mkpath(info.airplayDirectory()) || !dir.mkpath(info.presenterDirectory()) ||
        !dir.mkpath(info.exportsDirectory())) {
        return {{}, QStringLiteral("Could not create the local project folders")};
    }
    const QString error = writeManifest(info, document);
    return error.isEmpty() ? ProjectCreateResult{info, {}} : ProjectCreateResult{{}, error};
}

ProjectLoadResult ProjectStore::load(const QString &directory) const {
    const QString cleanDirectory = QDir::cleanPath(directory);
    QFile file(QDir(cleanDirectory).filePath(QStringLiteral("project.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {{}, nullptr, QStringLiteral("Could not open project.json")};
    }
    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        return {{}, nullptr, QStringLiteral("Project metadata is invalid: %1").arg(parseError.errorString())};
    }
    const QJsonObject root = json.object();
    ProjectInfo info;
    info.id = root.value(QStringLiteral("id")).toString();
    info.title = root.value(QStringLiteral("title")).toString();
    info.directory = cleanDirectory;
    info.state = projectStateFromKey(root.value(QStringLiteral("state")).toString());
    info.createdAtUtc = QDateTime::fromString(root.value(QStringLiteral("createdAtUtc")).toString(),
                                              Qt::ISODateWithMs);
    info.updatedAtUtc = QDateTime::fromString(root.value(QStringLiteral("updatedAtUtc")).toString(),
                                              Qt::ISODateWithMs);
    auto loaded = SceneDocument::fromJson(root.value(QStringLiteral("scene")).toObject());
    if (!loaded.has_value()) return {info, nullptr, loaded.error};
    return {info, std::move(loaded.document), {}};
}

QString ProjectStore::save(const ProjectInfo &project, const SceneDocument &document) const {
    ProjectInfo updated = project;
    const ProjectLoadResult current = load(project.directory);
    if (current.ok()) {
        updated.state = current.project.state;
        updated.createdAtUtc = current.project.createdAtUtc;
    }
    updated.title = document.title();
    updated.updatedAtUtc = QDateTime::currentDateTimeUtc();
    return writeManifest(updated, document);
}

QString ProjectStore::setState(const QString &directory, ProjectState state) {
    auto loaded = load(directory);
    if (!loaded.ok()) return loaded.error;
    loaded.project.state = state;
    loaded.project.updatedAtUtc = QDateTime::currentDateTimeUtc();
    return writeManifest(loaded.project, *loaded.document);
}

QList<ProjectSummary> ProjectStore::projects() const {
    QList<ProjectSummary> result;
    QDir root(m_rootDirectory);
    const QStringList directories = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                    QDir::Time | QDir::Reversed);
    for (const QString &name : directories) {
        auto loaded = load(root.filePath(name));
        if (loaded.ok()) result.append(summaryOf(loaded.project));
    }
    std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
        return left.updatedAtUtc > right.updatedAtUtc;
    });
    return result;
}

QList<ProjectSummary> ProjectStore::recoverableProjects() {
    QList<ProjectSummary> result;
    QDir root(m_rootDirectory);
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        auto loaded = load(root.filePath(name));
        if (!loaded.ok()) continue;
        if (loaded.project.state == ProjectState::Recording ||
            loaded.project.state == ProjectState::Finalizing ||
            loaded.project.state == ProjectState::Exporting) {
            loaded.project.state = ProjectState::Recoverable;
            loaded.project.updatedAtUtc = QDateTime::currentDateTimeUtc();
            if (!writeManifest(loaded.project, *loaded.document).isEmpty()) continue;
        }
        if (loaded.project.state == ProjectState::Recoverable) result.append(summaryOf(loaded.project));
    }
    return result;
}

QString ProjectStore::writeManifest(const ProjectInfo &project,
                                    const SceneDocument &document) const {
    QJsonObject root{{QStringLiteral("projectSchemaVersion"), 1},
                     {QStringLiteral("id"), project.id},
                     {QStringLiteral("title"), document.title()},
                     {QStringLiteral("state"), projectStateKey(project.state)},
                     {QStringLiteral("createdAtUtc"), project.createdAtUtc.toString(Qt::ISODateWithMs)},
                     {QStringLiteral("updatedAtUtc"), project.updatedAtUtc.toString(Qt::ISODateWithMs)},
                     {QStringLiteral("scene"), document.toJson()}};
    QSaveFile file(project.manifestPath());
    if (!file.open(QIODevice::WriteOnly)) return QStringLiteral("Could not write project.json");
    if (file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) < 0 || !file.commit()) {
        return QStringLiteral("Could not safely save project.json");
    }
    return {};
}

QString projectStateKey(ProjectState state) {
    switch (state) {
    case ProjectState::Ready: return QStringLiteral("ready");
    case ProjectState::Recording: return QStringLiteral("recording");
    case ProjectState::Finalizing: return QStringLiteral("finalizing");
    case ProjectState::Recoverable: return QStringLiteral("recoverable");
    case ProjectState::Exporting: return QStringLiteral("exporting");
    case ProjectState::Failed: return QStringLiteral("failed");
    }
    return QStringLiteral("ready");
}
