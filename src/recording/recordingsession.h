#pragma once

#include "projects/projectstore.h"

#include <QObject>
#include <QHash>
#include <QImage>
#include <QMutex>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <functional>

class PipelineRunner {
public:
    virtual ~PipelineRunner() = default;
    virtual bool startTrack(const QString &name, const QString &pipeline, QString *error) = 0;
    virtual bool stopAll(int timeoutMs, QString *error) = 0;
    virtual void setCameraPreviewCallback(std::function<void(const QImage &)> callback) {
        Q_UNUSED(callback)
    }
    virtual void setTrackFirstMediaCallback(
        std::function<void(const QString &, qint64)> callback) {
        Q_UNUSED(callback)
    }
};

enum class RecordingState { Idle, Starting, Recording, Finalizing, Failed };

struct RecordingOptions {
    bool systemAudio = true;
    bool camera = false;
    bool microphone = false;
};

class RecordingSession final : public QObject {
    Q_OBJECT
public:
    RecordingSession(ProjectStore *store, PipelineRunner *runner, QObject *parent = nullptr);
    ~RecordingSession() override;

    bool start(const ProjectInfo &project, const RecordingOptions &options);
    bool stop();
    void markAirplayFailure(const QString &message);
    RecordingState state() const;
    QString lastError() const;
    QStringList warnings() const;
    QStringList activeTracks() const;
    QString statusSummary() const;
    ProjectInfo project() const;

signals:
    void stateChanged(RecordingState state);
    void warningRaised(const QString &message);

private:
    static void observeAirplayFirstMedia(std::int64_t monotonicNanoseconds, void *context);
    void observeTrackFirstMedia(const QString &track, qint64 monotonicNanoseconds);
    void scheduleManifestRefresh();
    void transition(RecordingState state);
    bool writeSessionManifest(const QStringList &tracks);
    QHash<QString, qint64> measuredTrackOffsets(const QStringList &tracks) const;
    QString audioPipeline(bool loopback) const;
    QString cameraPipeline() const;

    ProjectStore *m_store = nullptr;
    PipelineRunner *m_runner = nullptr;
    ProjectInfo m_project;
    RecordingState m_state = RecordingState::Idle;
    QString m_lastError;
    QStringList m_warnings;
    QStringList m_activeTracks;
    QString m_startedAtUtc;
    mutable QMutex m_timingMutex;
    QHash<QString, qint64> m_firstMediaNanoseconds;
    bool m_manifestRefreshQueued = false;
    std::atomic_bool m_acceptTiming{false};
    bool m_airplayFailed = false;
};
