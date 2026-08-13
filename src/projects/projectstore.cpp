#include "projectstore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>
#include <algorithm>
#include <mutex>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>

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

bool hasUsableMatroskaStream(const QString &path, bool expectVideo) {
    static std::once_flag initializeGStreamer;
    std::call_once(initializeGStreamer, []() { gst_init(nullptr, nullptr); });

    GError *error = nullptr;
    GstDiscoverer *discoverer = gst_discoverer_new(2 * GST_SECOND, &error);
    if (!discoverer) {
        if (error) g_error_free(error);
        return false;
    }
    const QByteArray uri = QUrl::fromLocalFile(path).toEncoded(QUrl::FullyEncoded);
    GstDiscovererInfo *info = gst_discoverer_discover_uri(discoverer, uri.constData(), &error);
    bool usable = false;
    if (info && gst_discoverer_info_get_result(info) == GST_DISCOVERER_OK) {
        GList *streams = expectVideo ? gst_discoverer_info_get_video_streams(info)
                                     : gst_discoverer_info_get_audio_streams(info);
        usable = streams != nullptr;
        gst_discoverer_stream_info_list_free(streams);
    }
    if (info) gst_discoverer_info_unref(info);
    if (error) g_error_free(error);
    gst_object_unref(discoverer);
    return usable;
}

QString quarantinePath(const QString &path) {
    QString candidate = path + QStringLiteral(".incomplete");
    for (int suffix = 2; QFileInfo::exists(candidate); ++suffix)
        candidate = path + QStringLiteral(".incomplete-%1").arg(suffix);
    return candidate;
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
    QMutexLocker lock(&m_manifestMutex);
    const QString error = writeManifest(info, document);
    return error.isEmpty() ? ProjectCreateResult{info, {}} : ProjectCreateResult{{}, error};
}

ProjectLoadResult ProjectStore::load(const QString &directory) const {
    QMutexLocker lock(&m_manifestMutex);
    return loadUnlocked(directory);
}

ProjectLoadResult ProjectStore::loadUnlocked(const QString &directory) const {
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
    QMutexLocker lock(&m_manifestMutex);
    ProjectInfo updated = project;
    const ProjectLoadResult current = loadUnlocked(project.directory);
    if (current.ok()) {
        updated.state = current.project.state;
        updated.createdAtUtc = current.project.createdAtUtc;
    }
    updated.title = document.title();
    updated.updatedAtUtc = QDateTime::currentDateTimeUtc();
    return writeManifest(updated, document);
}

QString ProjectStore::setState(const QString &directory, ProjectState state) {
    QMutexLocker lock(&m_manifestMutex);
    auto loaded = loadUnlocked(directory);
    if (!loaded.ok()) return loaded.error;
    loaded.project.state = state;
    loaded.project.updatedAtUtc = QDateTime::currentDateTimeUtc();
    return writeManifest(loaded.project, *loaded.document);
}

ProjectRecoveryResult ProjectStore::recover(const QString &directory,
                                            const std::function<bool()> &cancelled) {
    QMutexLocker lock(&m_manifestMutex);
    if (cancelled && cancelled()) return {0, 0, QStringLiteral("Recovery check cancelled")};
    auto loaded = loadUnlocked(directory);
    if (!loaded.ok()) return {0, 0, loaded.error};
    if (loaded.project.state != ProjectState::Recoverable) {
        return {0, 0, QStringLiteral("Only an interrupted Recoverable project can be recovered")};
    }

    ProjectRecoveryResult result;
    bool hasAirplayVideo = false;
    const QList<QPair<QString, QStringList>> mediaLocations {
        {loaded.project.airplayDirectory(),
         {QStringLiteral("video-*.mkv"), QStringLiteral("audio-*.mka")}},
        {loaded.project.presenterDirectory(),
         {QStringLiteral("camera-*.mkv"), QStringLiteral("microphone-*.mka")}}
    };
    for (const auto &location : mediaLocations) {
        QDir mediaDirectory(location.first);
        const QStringList files = mediaDirectory.entryList(location.second, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            if (cancelled && cancelled()) {
                result.error = QStringLiteral("Recovery check cancelled; the project remains Recoverable");
                return result;
            }
            const QString path = mediaDirectory.filePath(name);
            const bool videoFragment = name.endsWith(QStringLiteral(".mkv"), Qt::CaseInsensitive);
            if (hasUsableMatroskaStream(path, videoFragment)) {
                ++result.usableMediaFiles;
                if (location.first == loaded.project.airplayDirectory()
                    && name.startsWith(QStringLiteral("video-"))) {
                    hasAirplayVideo = true;
                }
                continue;
            }
            if (!QFile::rename(path, quarantinePath(path))) {
                result.error = QStringLiteral("Could not preserve an incomplete media fragment: %1")
                    .arg(name);
                return result;
            }
            ++result.quarantinedMediaFiles;
        }
    }
    if (!hasAirplayVideo) {
        result.error = QStringLiteral("No usable AirPlay video fragment was found; the project remains Recoverable");
        return result;
    }

    loaded.project.state = ProjectState::Ready;
    loaded.project.updatedAtUtc = QDateTime::currentDateTimeUtc();
    result.error = writeManifest(loaded.project, *loaded.document);
    return result;
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
    QMutexLocker lock(&m_manifestMutex);
    QList<ProjectSummary> result;
    QDir root(m_rootDirectory);
    for (const QString &name : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        auto loaded = loadUnlocked(root.filePath(name));
        if (!loaded.ok()) continue;
        if (loaded.project.state == ProjectState::Exporting) {
            bool partialsRemoved = true;
            QDir exports(loaded.project.exportsDirectory());
            for (const QString &partial : exports.entryList(
                     {QStringLiteral("*.partial")}, QDir::Files)) {
                if (!exports.remove(partial)) partialsRemoved = false;
            }
            if (!partialsRemoved) continue;
            loaded.project.state = ProjectState::Ready;
            loaded.project.updatedAtUtc = QDateTime::currentDateTimeUtc();
            if (!writeManifest(loaded.project, *loaded.document).isEmpty()) continue;
        } else if (loaded.project.state == ProjectState::Recording ||
                   loaded.project.state == ProjectState::Finalizing) {
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
