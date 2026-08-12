#pragma once

#include "exportpipeline.h"

#include <QObject>

class QThread;

class ExportJob final : public QObject {
    Q_OBJECT
public:
    explicit ExportJob(ProjectStore *store, QObject *parent = nullptr);
    ~ExportJob() override;
    bool start(const ProjectInfo &project, const SceneDocument &scene,
               SceneFormat format, const QString &outputPath);
    bool isRunning() const;

signals:
    void started(const QString &outputPath);
    void finished(const QString &outputPath);
    void failed(const QString &message);

private:
    ProjectStore *m_store = nullptr;
    QThread *m_thread = nullptr;
};
