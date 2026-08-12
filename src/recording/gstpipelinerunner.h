#pragma once

#include "recordingsession.h"

#include <QList>
#include <QString>

typedef struct _GstElement GstElement;

class GstPipelineRunner final : public PipelineRunner {
public:
    GstPipelineRunner();
    ~GstPipelineRunner() override;
    bool startTrack(const QString &name, const QString &pipeline, QString *error) override;
    bool stopAll(int timeoutMs, QString *error) override;

private:
    struct Track { QString name; GstElement *pipeline = nullptr; };
    QList<Track> m_tracks;
};
