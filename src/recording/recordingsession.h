#pragma once

#include "projects/projectstore.h"

#include <QObject>
#include <QRect>
#include <QStringList>

class PipelineRunner {
public:
    virtual ~PipelineRunner() = default;
    virtual bool startTrack(const QString &name, const QString &pipeline, QString *error) = 0;
    virtual bool stopAll(int timeoutMs, QString *error) = 0;
};

enum class RecordingState { Idle, Starting, Recording, Finalizing, Failed };

struct RecordingOptions {
    QRect captureRect;
    quint64 monitorHandle = 0;
    bool systemAudio = true;
    bool camera = false;
    bool microphone = false;
    int frameRate = 60;
};

class RecordingSession final : public QObject {
    Q_OBJECT
public:
    RecordingSession(ProjectStore *store, PipelineRunner *runner, QObject *parent = nullptr);
    ~RecordingSession() override;

    bool start(const ProjectInfo &project, const RecordingOptions &options);
    bool stop();
    RecordingState state() const;
    QString lastError() const;
    QStringList warnings() const;
    ProjectInfo project() const;

signals:
    void stateChanged(RecordingState state);
    void warningRaised(const QString &message);

private:
    void transition(RecordingState state);
    bool writeSessionManifest(const RecordingOptions &options, const QStringList &tracks);
    QString videoPipeline(const RecordingOptions &options) const;
    QString audioPipeline(bool loopback) const;
    QString cameraPipeline() const;

    ProjectStore *m_store = nullptr;
    PipelineRunner *m_runner = nullptr;
    ProjectInfo m_project;
    RecordingState m_state = RecordingState::Idle;
    QString m_lastError;
    QStringList m_warnings;
};
