#pragma once

#include "projects/projectstore.h"

#include <QHash>
#include <QSize>

struct ExportPipelineResult {
    QString description;
    QString error;
    bool ok() const { return error.isEmpty() && !description.isEmpty(); }
};

class ExportPipeline final {
public:
    static QHash<QString, qint64> trackStartOffsets(const ProjectInfo &project);
    static qint64 alignedTimelineDuration(
        const QHash<QString, qint64> &trackDurationsNanoseconds,
        const QHash<QString, qint64> &trackStartOffsetsNanoseconds);
    static ExportPipelineResult build(const ProjectInfo &project, const SceneDocument &scene,
                                      SceneFormat format, const QString &outputPath,
                                      qint64 durationNanoseconds = 0,
                                      const QHash<QString, QSize> &sourcePixelSizes = {});
};
