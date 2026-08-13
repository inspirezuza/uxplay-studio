#include "recordingsession.h"
#include "uxplay_api.h"

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QMutexLocker>
#include <QSaveFile>
#include <algorithm>
#include <limits>

namespace {
QString quoted(const QString &path) {
    QString escaped = QDir::toNativeSeparators(path);
    escaped.replace('\\', QStringLiteral("\\\\"));
    escaped.replace('"', QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}
}

RecordingSession::RecordingSession(ProjectStore *store, PipelineRunner *runner, QObject *parent)
    : QObject(parent), m_store(store), m_runner(runner) {
    if (m_runner) {
        m_runner->setTrackFirstMediaCallback(
            [this](const QString &track, qint64 monotonicNanoseconds) {
                observeTrackFirstMedia(track, monotonicNanoseconds);
            });
    }
    uxplay_set_recording_first_media_callback(&RecordingSession::observeAirplayFirstMedia,
                                               this);
}

RecordingSession::~RecordingSession() {
    m_acceptTiming = false;
    uxplay_set_recording_first_media_callback(nullptr, nullptr);
    if (m_runner) m_runner->setTrackFirstMediaCallback({});
    if (m_state == RecordingState::Recording || m_state == RecordingState::Starting ||
        m_state == RecordingState::Finalizing) {
        QString ignored;
        m_runner->stopAll(700, &ignored);
        uxplay_stop_recording();
        m_store->setState(m_project.directory, ProjectState::Recoverable);
    }
}

bool RecordingSession::start(const ProjectInfo &project, const RecordingOptions &options) {
    if ((m_state != RecordingState::Idle && m_state != RecordingState::Failed) || !m_store || !m_runner) {
        m_lastError = QStringLiteral("The recording target is not ready");
        return false;
    }
    m_project = project;
    m_lastError.clear();
    m_warnings.clear();
    m_activeTracks.clear();
    m_startedAtUtc = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    {
        QMutexLocker lock(&m_timingMutex);
        m_firstMediaNanoseconds.clear();
        m_manifestRefreshQueued = false;
    }
    m_airplayFailed = false;
    m_acceptTiming = true;
    transition(RecordingState::Starting);
    if (!m_store->setState(project.directory, ProjectState::Recording).isEmpty()) {
        m_acceptTiming = false;
        m_lastError = QStringLiteral("Could not mark the project as recording");
        transition(RecordingState::Failed);
        return false;
    }

    QStringList tracks;
    const QByteArray airplayDirectory = QDir::toNativeSeparators(m_project.airplayDirectory()).toUtf8();
    if (!uxplay_start_recording(airplayDirectory.constData())) {
        m_acceptTiming = false;
        m_lastError = QStringLiteral("Could not start the direct AirPlay video track");
        m_store->setState(project.directory, ProjectState::Ready);
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
    m_activeTracks = tracks;
    if (!writeSessionManifest(tracks)) {
        m_acceptTiming = false;
        m_lastError = QStringLiteral("Could not safely write session metadata");
        QString ignored;
        m_runner->stopAll(1000, &ignored);
        uxplay_stop_recording();
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
    QString optionalError;
    const bool optionalTracksClean = m_runner->stopAll(5000, &optionalError);
    const bool airplayStopClean = uxplay_stop_recording() != 0;
    const bool airplayTrackClean = airplayStopClean && !m_airplayFailed;
    m_acceptTiming = false;
    const bool manifestClean = writeSessionManifest(m_activeTracks);
    const bool clean = optionalTracksClean && airplayTrackClean && manifestClean;
    QStringList errors;
    if (m_airplayFailed && !m_lastError.isEmpty()) errors.append(m_lastError);
    if (!optionalError.isEmpty()) errors.append(optionalError);
    if (!airplayTrackClean) {
        const QString airplayError = QStringLiteral("The AirPlay video track could not be finalized safely");
        if (!errors.contains(airplayError)) errors.append(airplayError);
    }
    if (!manifestClean)
        errors.append(QStringLiteral("Could not safely update session timing metadata"));
    m_lastError = errors.join(QStringLiteral("; "));
    m_store->setState(m_project.directory, clean ? ProjectState::Ready : ProjectState::Recoverable);
    transition(clean ? RecordingState::Idle : RecordingState::Failed);
    return clean;
}

void RecordingSession::markAirplayFailure(const QString &message) {
    if (m_state != RecordingState::Recording && m_state != RecordingState::Starting) return;
    m_airplayFailed = true;
    if (!message.isEmpty()) m_lastError = message;
    const QString warning = message.isEmpty()
        ? QStringLiteral("The AirPlay recording track failed; stop to preserve recoverable media")
        : message;
    if (!m_warnings.contains(warning)) {
        m_warnings.append(warning);
        emit warningRaised(warning);
    }
}

RecordingState RecordingSession::state() const { return m_state; }
QString RecordingSession::lastError() const { return m_lastError; }
QStringList RecordingSession::warnings() const { return m_warnings; }
QStringList RecordingSession::activeTracks() const { return m_activeTracks; }
QString RecordingSession::statusSummary() const {
    switch (m_state) {
    case RecordingState::Starting:
        return QStringLiteral("Starting independent tracks...");
    case RecordingState::Recording:
        if (m_airplayFailed)
            return QStringLiteral("Recording issue - stop now to preserve recoverable media");
        if (!m_warnings.isEmpty()) {
            return QStringLiteral("REC - %1 active - %2 unavailable")
                .arg(m_activeTracks.size()).arg(m_warnings.size());
        }
        return QStringLiteral("REC - %1 independent tracks").arg(m_activeTracks.size());
    case RecordingState::Finalizing:
        return QStringLiteral("Finalizing media safely...");
    case RecordingState::Failed:
        return m_lastError.isEmpty()
            ? QStringLiteral("Recording needs attention")
            : QStringLiteral("Recording needs attention - %1").arg(m_lastError);
    case RecordingState::Idle:
        return QStringLiteral("Ready to record");
    }
    return QStringLiteral("Ready to record");
}
ProjectInfo RecordingSession::project() const { return m_project; }

void RecordingSession::observeAirplayFirstMedia(std::int64_t monotonicNanoseconds,
                                                void *context) {
    auto *session = static_cast<RecordingSession *>(context);
    if (session) {
        session->observeTrackFirstMedia(QStringLiteral("airplay-video"),
                                        static_cast<qint64>(monotonicNanoseconds));
    }
}

void RecordingSession::observeTrackFirstMedia(const QString &track,
                                              qint64 monotonicNanoseconds) {
    if (!m_acceptTiming.load() || monotonicNanoseconds < 0) return;
    bool inserted = false;
    {
        QMutexLocker lock(&m_timingMutex);
        if (!m_firstMediaNanoseconds.contains(track)) {
            m_firstMediaNanoseconds.insert(track, monotonicNanoseconds);
            inserted = true;
        }
    }
    if (inserted) scheduleManifestRefresh();
}

void RecordingSession::scheduleManifestRefresh() {
    {
        QMutexLocker lock(&m_timingMutex);
        if (m_manifestRefreshQueued) return;
        m_manifestRefreshQueued = true;
    }
    QMetaObject::invokeMethod(this, [this]() {
        {
            QMutexLocker lock(&m_timingMutex);
            m_manifestRefreshQueued = false;
        }
        if (m_state == RecordingState::Starting || m_state == RecordingState::Recording ||
            m_state == RecordingState::Finalizing) {
            writeSessionManifest(m_activeTracks);
        }
    }, Qt::QueuedConnection);
}

void RecordingSession::transition(RecordingState state) {
    if (m_state == state) return;
    m_state = state;
    emit stateChanged(state);
}

QHash<QString, qint64> RecordingSession::measuredTrackOffsets(
    const QStringList &tracks) const {
    QMutexLocker lock(&m_timingMutex);
    qint64 origin = std::numeric_limits<qint64>::max();
    for (const QString &track : tracks) {
        const auto it = m_firstMediaNanoseconds.constFind(track);
        if (it != m_firstMediaNanoseconds.cend()) origin = std::min(origin, it.value());
    }
    QHash<QString, qint64> offsets;
    if (origin == std::numeric_limits<qint64>::max()) return offsets;
    for (const QString &track : tracks) {
        const auto it = m_firstMediaNanoseconds.constFind(track);
        if (it != m_firstMediaNanoseconds.cend()) offsets.insert(track, it.value() - origin);
    }
    return offsets;
}

bool RecordingSession::writeSessionManifest(const QStringList &tracks) {
    const QHash<QString, qint64> trackOffsetsNanoseconds = measuredTrackOffsets(tracks);
    QJsonArray jsonTracks;
    for (const QString &track : tracks) jsonTracks.append(track);
    QJsonObject jsonTrackOffsets;
    for (auto it = trackOffsetsNanoseconds.cbegin(); it != trackOffsetsNanoseconds.cend(); ++it)
        jsonTrackOffsets.insert(it.key(), static_cast<double>(it.value()));
    QJsonObject root{{QStringLiteral("sessionSchemaVersion"), 1},
                     {QStringLiteral("startedAtUtc"), m_startedAtUtc},
                     {QStringLiteral("tracks"), jsonTracks},
                     {QStringLiteral("trackStartOffsetsNanoseconds"), jsonTrackOffsets}};
    QSaveFile file(QDir(m_project.directory).filePath(QStringLiteral("session.json")));
    return file.open(QIODevice::WriteOnly) &&
           file.write(QJsonDocument(root).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
}

QString RecordingSession::audioPipeline(bool loopback) const {
    const QString folder = loopback ? m_project.airplayDirectory() : m_project.presenterDirectory();
    const QString prefix = loopback ? QStringLiteral("audio") : QStringLiteral("microphone");
    const QString location = QDir(folder).filePath(prefix + QStringLiteral("-%05d.mka"));
    QString pipeline = QStringLiteral(
        "wasapi2src do-timestamp=true low-latency=true loopback=%1 ! queue "
        "! identity name=studio-track-origin silent=true ! audioconvert ! audioresample "
        "! audio/x-raw,rate=48000,channels=2 ! avenc_aac bitrate=192000 ! aacparse ! queue ! smux.audio_0 "
        "splitmuxsink name=smux muxer-factory=matroskamux max-size-time=30000000000 location=")
        .arg(loopback ? QStringLiteral("true") : QStringLiteral("false"));
    pipeline += quoted(location) + QStringLiteral(" async-finalize=true");
    return pipeline;
}

QString RecordingSession::cameraPipeline() const {
    const QString location = QDir(m_project.presenterDirectory()).filePath(QStringLiteral("camera-%05d.mkv"));
    return QStringLiteral(
        "mfvideosrc do-timestamp=true ! queue leaky=downstream max-size-buffers=3 "
        "! identity name=studio-track-origin silent=true ! videoconvert ! videoscale "
        "! tee name=studio-camera-tee "
        "studio-camera-tee. ! queue ! video/x-raw,format=NV12,framerate=30/1 "
        "! mfh264enc bitrate=6000 low-latency=true rc-mode=cbr quality-vs-speed=15 "
        "! h264parse ! splitmuxsink muxer-factory=matroskamux max-size-time=30000000000 location=")
        + quoted(location) + QStringLiteral(
            " async-finalize=true studio-camera-tee. ! queue leaky=downstream max-size-buffers=2 "
            "! videorate ! videoscale ! videoconvert "
            "! video/x-raw,format=BGRA,width=640,height=360,framerate=15/1 "
            "! appsink name=studio-camera-preview max-buffers=1 drop=true sync=false");
}
