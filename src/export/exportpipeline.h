#pragma once

#include "projects/projectstore.h"

struct ExportPipelineResult {
    QString description;
    QString error;
    bool ok() const { return error.isEmpty() && !description.isEmpty(); }
};

class ExportPipeline final {
public:
    static ExportPipelineResult build(const ProjectInfo &project, const SceneDocument &scene,
                                      SceneFormat format, const QString &outputPath,
                                      qint64 durationNanoseconds = 0);
};
