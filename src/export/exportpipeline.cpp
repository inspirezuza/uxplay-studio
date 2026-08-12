#include "exportpipeline.h"

#include <QDir>
#include <QColor>
#include <QFileInfo>
#include <QUrl>
#include <QtMath>

namespace {
QString quote(QString value) {
    value.replace('\\', QStringLiteral("\\\\"));
    value.replace('"', QStringLiteral("\\\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString firstPattern(const QString &directory, const QString &prefix, const QString &suffix) {
    return QDir(directory).filePath(prefix + QStringLiteral("-*.") + suffix);
}

bool hasTrack(const QString &directory, const QString &prefix, const QString &suffix) {
    return !QDir(directory).entryList({prefix + QStringLiteral("-*.") + suffix}, QDir::Files).isEmpty();
}
}

ExportPipelineResult ExportPipeline::build(const ProjectInfo &project,
                                           const SceneDocument &scene,
                                           SceneFormat format,
                                           const QString &outputPath,
                                           qint64 durationNanoseconds) {
    const SceneComposition &composition = scene.composition(format);
    if (composition.layers.isEmpty()) return {{}, QStringLiteral("The scene has no layers")};
    QStringList inputs;
    int pad = 0;
    for (const SceneLayer &layer : composition.layers) {
        if (!layer.visible || layer.transform.opacity <= 0) continue;
        const SceneSource *source = scene.source(layer.sourceId);
        if (!source) continue;
        const QRectF frame = layer.transform.frame;
        QString sourcePipeline;
        const bool isStatic = source->type == SceneSourceType::Image ||
                              source->type == SceneSourceType::Color ||
                              source->type == SceneSourceType::Text;
        const qint64 staticFrames = durationNanoseconds > 0
            ? qMax<qint64>(1, durationNanoseconds * 60 / 1'000'000'000)
            : 0;
        if (isStatic && staticFrames == 0) {
            return {{}, QStringLiteral("The recording timeline duration is unavailable")};
        }
        if (source->type == SceneSourceType::AirPlay &&
            hasTrack(project.airplayDirectory(), QStringLiteral("video"), QStringLiteral("mkv"))) {
            sourcePipeline = QStringLiteral("splitmuxsrc location=%1 ! decodebin")
                .arg(quote(firstPattern(project.airplayDirectory(), "video", "mkv")));
        } else if (source->type == SceneSourceType::Camera &&
                   hasTrack(project.presenterDirectory(), QStringLiteral("camera"), QStringLiteral("mkv"))) {
            sourcePipeline = QStringLiteral("splitmuxsrc location=%1 ! decodebin")
                .arg(quote(firstPattern(project.presenterDirectory(), "camera", "mkv")));
        } else if (source->type == SceneSourceType::Image && QFileInfo::exists(source->uri)) {
            sourcePipeline = QStringLiteral(
                "filesrc location=%1 ! decodebin ! imagefreeze num-buffers=%2 "
                "! video/x-raw,framerate=60/1")
                .arg(quote(source->uri)).arg(staticFrames);
        } else if (source->type == SceneSourceType::Color) {
            const QColor color(QColor::isValidColorName(source->uri) ? source->uri : QStringLiteral("#24345c"));
            sourcePipeline = QStringLiteral(
                "videotestsrc pattern=solid-color foreground-color=%1 is-live=false num-buffers=%2 "
                "! video/x-raw,framerate=60/1")
                .arg(color.rgba()).arg(staticFrames);
        } else if (source->type == SceneSourceType::Text) {
            sourcePipeline = QStringLiteral(
                "videotestsrc pattern=black is-live=false num-buffers=%1 "
                "! video/x-raw,framerate=60/1 ! textoverlay text=%2 "
                "valignment=center halignment=center")
                .arg(staticFrames).arg(quote(source->name));
        } else {
            continue;
        }
        const int width = qMax(2, static_cast<int>(frame.width()) & ~1);
        const int height = qMax(2, static_cast<int>(frame.height()) & ~1);
        const int cropLeft = qBound(0, qRound(width * layer.transform.crop.left()), width - 2);
        const int cropRight = qBound(0, qRound(width * layer.transform.crop.right()), width - cropLeft - 2);
        const int cropTop = qBound(0, qRound(height * layer.transform.crop.top()), height - 2);
        const int cropBottom = qBound(0, qRound(height * layer.transform.crop.bottom()), height - cropTop - 2);
        QString effects = QStringLiteral(" ! videocrop left=%1 right=%2 top=%3 bottom=%4")
            .arg(cropLeft).arg(cropRight).arg(cropTop).arg(cropBottom);
        if (!qFuzzyIsNull(layer.transform.rotationDegrees)) {
            effects += QStringLiteral(" ! videoconvert ! video/x-raw,format=BGRA ! rotate angle=%1")
                .arg(qDegreesToRadians(layer.transform.rotationDegrees), 0, 'f', 8);
        }
        if (layer.transform.mask != SceneMask::None) {
            effects += QStringLiteral(" ! videoconvert ! video/x-raw,format=BGRA ! cairooverlay name=%1-mask-%2")
                .arg(layer.transform.mask == SceneMask::Circle ? QStringLiteral("circle")
                                                               : QStringLiteral("rounded"))
                .arg(pad);
        }
        inputs << QStringLiteral("%1 ! queue ! videoconvert ! videoscale ! video/x-raw,width=%2,height=%3%4 ! comp.sink_%5 ")
            .arg(sourcePipeline).arg(width).arg(height).arg(effects).arg(pad);
        inputs << QStringLiteral("compositor name=comp sink_%1::xpos=%2 sink_%1::ypos=%3 sink_%1::alpha=%4 ")
            .arg(pad).arg(qRound(frame.x()) + cropLeft).arg(qRound(frame.y()) + cropTop)
            .arg(layer.transform.opacity, 0, 'f', 3);
        ++pad;
    }
    if (pad == 0) return {{}, QStringLiteral("No recorded or supported visible layers are available")};

    // Keep one compositor declaration: replace duplicate declarations emitted for readable pad specs.
    QString declarations;
    QString branches;
    for (int i = 0; i < inputs.size(); i += 2) {
        branches += inputs.at(i);
        const QString spec = inputs.at(i + 1);
        declarations += spec.mid(spec.indexOf(QStringLiteral("sink_")));
    }
    const QSize size = composition.canvasSize;
    QString pipeline = branches + QStringLiteral("compositor name=comp background=black %1 ! videoconvert ! video/x-raw,format=NV12,width=%2,height=%3,framerate=60/1 ! mfh264enc bitrate=12000 low-latency=false rc-mode=qvbr qp=22 quality-vs-speed=65 ! h264parse ! queue ! mux.video_0 mp4mux name=mux faststart=true ! filesink location=%4 ")
        .arg(declarations).arg(size.width()).arg(size.height()).arg(quote(outputPath));

    QStringList audio;
    if (hasTrack(project.airplayDirectory(), "audio", "mka"))
        audio << QStringLiteral("splitmuxsrc location=%1 ! decodebin ! audioconvert ! audioresample ! mix.")
                    .arg(quote(firstPattern(project.airplayDirectory(), "audio", "mka")));
    if (hasTrack(project.presenterDirectory(), "microphone", "mka"))
        audio << QStringLiteral("splitmuxsrc location=%1 ! decodebin ! audioconvert ! audioresample ! mix.")
                    .arg(quote(firstPattern(project.presenterDirectory(), "microphone", "mka")));
    if (!audio.isEmpty()) {
        pipeline += audio.join(' ') + QStringLiteral(" audiomixer name=mix ! avenc_aac bitrate=192000 ! aacparse ! queue ! mux.audio_0");
    }
    return {pipeline, {}};
}
