#include "recordingsession.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace {
QString quoted(const QString &path) {
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace('\\', QStringLiteral("\\\\"));
    escaped.replace('"', QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}
}

RecordingSession::RecordingSession(ProjectStore *store, PipelineRunner *runner, QObject *parent)
    : QObject(parent), m_store(store), m_runner(runner) {}

RecordingSession::~RecordingSession() {
    if (m_state == RecordingState::Recording || m_state == RecordingState::Starting ||
        m_state == RecordingState::Finalizing) {
        QString ignored;
        m_runner->stopAll(700, &ignored);
        m_store->setState(m_project.directory, ProjectState::Recoverable);
    }
}

bool RecordingSession::start(const ProjectInfo &project, const RecordingOptions &options) {
    if ((m_state != RecordingState::Idle && m_state != RecordingState::Failed) || !m_store || !m_runner ||
        options.captureRect.width() < 16 || options.captureRect.height() < 16) {
        m_lastError = QStringLiteral("The recording target is not ready");
        return false;
    }
    m_project = project;
    m_lastError.clear();
    m_warnings.clear();
    transition(RecordingState::Starting);
    if (!m_store->setState(project.directory, ProjectState::Recording).isEmpty()) {
        m_lastError = QStringLiteral("Could not mark the project as recording");
        transition(RecordingState::Failed);
        return false;
    }

    QString error;
    QStringList tracks;
    if (!m_runner->startTrack(QStringLiteral("airplay-video"), videoPipeline(options), &error)) {
        m_lastError = error.isEmpty() ? QStringLiteral("Could not start the AirPlay video track") : error;
        m_store->setState(project.directory, ProjectState::Failed);
        transition(RecordingState::Failed);
        transition(RecordingState::Idle);
        return false;
    }
    tracks.append(QStringLiteral("airplay-video"));

    const auto optional = [this, &tracks](const QString &name, const QString &pipeline) {
        QString error;
        if (m_runner->startTrack(name, pipeline, &error)) {
            tracks.append(name);
        } else {
            const QString warning = QStringLiteral("%1 track is unavailable%2")
                .arg(name, error.isEmpty() ? QString() : QStringLiteral(": ") + error);
            m_warnings.append(warning);
            emit warningRaised(warning);
        }
    };
    if (options.systemAudio) optional(QStringLiteral("airplay-audio"), audioPipeline(true));
    if (options.camera) optional(QStringLiteral("camera"), cameraPipeline());
    if (options.microphone) optional(QStringLiteral("microphone"), audioPipeline(false));
    if (!writeSessionManifest(options, tracks)) {
        m_lastError = QStringLiteral("Could not safely write session metadata");
        QString ignored;
        m_runner->stopAll(1000, &ignored);
        m_store->setState(project.directory, ProjectState::Recoverable);
        transition(RecordingState::Idle);
        return false;
    }
    transition(RecordingState::Recording);
    return true;
}

bool RecordingSession::stop() {
    if (m_state != RecordingState::Recording) return false;
    transition(RecordingState::Finalizing);
    m_store->setState(m_project.directory, ProjectState::Finalizing);
    QString error;
    const bool clean = m_runner->stopAll(5000, &error);
    m_lastError = error;
    m_store->setState(m_project.directory, clean ? ProjectState::Ready : ProjectState::Recoverable);
    transition(clean ? RecordingState::Idle : RecordingState::Failed);
    return clean;
}

RecordingState RecordingSession::state() const { return m_state; }
QString RecordingSession::lastError() const { return m_lastError; }
QStringList RecordingSession::warnings() const { return m_warnings; }
ProjectInfo RecordingSession::project() const { return m_project; }

void RecordingSession::transition(RecordingState state) {
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
}

bool RecordingSession::writeSessionManifest(const RecordingOptions &options,
                                             const QStringList &tracks) {
    QJsonArray jsonTracks;
    for (const QString &track : tracks) jsonTracks.append(track);
    QJsonObject root{{QStringLiteral("sessionSchemaVersion"), 1},
                     {QStringLiteral("startedAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                     {QStringLiteral("tracks"), jsonTracks},
                     {QStringLiteral("captureX"), options.captureRect.x()},
                     {QStringLiteral("captureY"), options.captureRect.y()},
                     {QStringLiteral("captureWidth"), options.captureRect.width()},
                     {QStringLiteral("captureHeight"), options.captureRect.height()},
                     {QStringLiteral("frameRate"), options.frameRate}};
    QSaveFile file(QDir(m_project.directory).filePath(QStringLiteral("session.json")));
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
}

QString RecordingSession::videoPipeline(const RecordingOptions &options) const {
    const QRect r = options.captureRect;
    const QString location = QDir(m_project.airplayDirectory()).filePath(QStringLiteral("video-%05d.mkv"));
    QString pipeline = QStringLiteral("d3d11screencapturesrc do-timestamp=true show-cursor=false ");
    if (options.monitorHandle) pipeline += QStringLiteral("monitor-handle=%1 ").arg(options.monitorHandle);
    pipeline += QStringLiteral("crop-x=%1 crop-y=%2 crop-width=%3 crop-height=%4 ")
        .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
    pipeline += QStringLiteral(
        "! queue leaky=downstream max-size-buffers=3 ! d3d11convert ! videoscale "
        "! video/x-raw(memory:D3D11Memory),format=NV12,framerate=%1/1 ! mfh264enc bitrate=12000 low-latency=true rc-mode=cbr quality-vs-speed=15 "
        "! h264parse ! splitmuxsink muxer-factory=matroskamux max-size-time=30000000000 location=")
        .arg(options.frameRate);
    pipeline += quoted(location) + QStringLiteral(" async-finalize=true");
    return pipeline;
}

QString RecordingSession::audioPipeline(bool loopback) const {
    const QString folder = loopback ? m_project.airplayDirectory() : m_project.presenterDirectory();
    const QString prefix = loopback ? QStringLiteral("audio") : QStringLiteral("microphone");
    const QString location = QDir(folder).filePath(prefix + QStringLiteral("-%05d.mka"));
    QString pipeline = QStringLiteral(
        "wasapi2src do-timestamp=true low-latency=true loopback=%1 ! queue ! audioconvert ! audioresample "
        "! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=192000 ! aacparse ! queue ! smux.audio_0 "
        "splitmuxsink name=smux muxer-factory=matroskamux max-size-time=30000000000 location=")
        .arg(loopback ? QStringLiteral("true") : QStringLiteral("false"));
    pipeline += quoted(location) + QStringLiteral(" async-finalize=true");
    return pipeline;
}

QString RecordingSession::cameraPipeline() const {
    const QString location = QDir(m_project.presenterDirectory()).filePath(QStringLiteral("camera-%05d.mkv"));
    return QStringLiteral(
        "mfvideosrc do-timestamp=true ! queue leaky=downstream max-size-buffers=3 ! videoconvert ! videoscale "
        "! video/x-raw,format=NV12,framerate=30/1 ! mfh264enc bitrate=6000 low-latency=true rc-mode=cbr quality-vs-speed=15 "
        "! h264parse ! splitmuxsink muxer-factory=matroskamux max-size-time=30000000000 location=")
        + quoted(location) + QStringLiteral(" async-finalize=true");
}
