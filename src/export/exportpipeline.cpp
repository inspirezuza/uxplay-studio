#include "exportpipeline.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <QtMath>

#include <limits>

namespace {
struct PixelCrop {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
    int visibleWidth = 0;
    int visibleHeight = 0;
};

QString quote(QString value) {
    value.replace('\\', QStringLiteral("\\\\"));
    value.replace('"', QStringLiteral("\\\""));
    return QStringLiteral("\"") + value + QStringLiteral("\"");
}

QString firstPattern(const QString &directory, const QString &prefix, const QString &suffix) {
    return QDir(directory).filePath(prefix + QStringLiteral("-*.") + suffix);
}

QString withTimestampOffset(QString pipeline, qint64 offsetNanoseconds) {
    return pipeline + QStringLiteral(" ! identity ts-offset=%1").arg(offsetNanoseconds);
}

QString videoSource(const QString &directory, const QString &prefix, qint64 offsetNanoseconds) {
    const QStringList segments = QDir(directory).entryList(
        {prefix + QStringLiteral("-*.mkv")}, QDir::Files, QDir::Name);
    if (segments.size() == 1) {
        return withTimestampOffset(
            QStringLiteral("filesrc location=%1 ! decodebin")
                .arg(quote(QDir(directory).filePath(segments.first()))),
            offsetNanoseconds);
    }
    return withTimestampOffset(
        QStringLiteral("splitmuxsrc location=%1 ! decodebin")
            .arg(quote(firstPattern(directory, prefix, QStringLiteral("mkv")))),
        offsetNanoseconds);
}

bool hasTrack(const QString &directory, const QString &prefix, const QString &suffix) {
    return !QDir(directory).entryList({prefix + QStringLiteral("-*.") + suffix}, QDir::Files).isEmpty();
}

QHash<QString, qint64> readTrackStartOffsets(const ProjectInfo &project) {
    QFile manifest(QDir(project.directory).filePath(QStringLiteral("session.json")));
    if (!manifest.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(manifest.readAll());
    if (!document.isObject()) return {};
    const QJsonObject offsets = document.object()
        .value(QStringLiteral("trackStartOffsetsNanoseconds")).toObject();
    QHash<QString, qint64> result;
    for (auto it = offsets.constBegin(); it != offsets.constEnd(); ++it) {
        if (!it.value().isDouble()) continue;
        const double value = it.value().toDouble();
        if (!qIsFinite(value) || value < static_cast<double>(std::numeric_limits<qint64>::min()) ||
            value > static_cast<double>(std::numeric_limits<qint64>::max())) continue;
        result.insert(it.key(), qRound64(value));
    }
    return result;
}

bool hasCrop(const QMarginsF &crop) {
    return crop.left() > 0.0 || crop.right() > 0.0 ||
           crop.top() > 0.0 || crop.bottom() > 0.0;
}

PixelCrop cropPixels(const QSize &size, const QMarginsF &crop,
                     int minimumVisible, bool evenVisible) {
    PixelCrop result;
    result.left = qBound(0, qRound(size.width() * crop.left()),
                         size.width() - minimumVisible);
    result.right = qBound(0, qRound(size.width() * crop.right()),
                          size.width() - result.left - minimumVisible);
    result.top = qBound(0, qRound(size.height() * crop.top()),
                        size.height() - minimumVisible);
    result.bottom = qBound(0, qRound(size.height() * crop.bottom()),
                           size.height() - result.top - minimumVisible);
    result.visibleWidth = size.width() - result.left - result.right;
    result.visibleHeight = size.height() - result.top - result.bottom;
    if (evenVisible) {
        result.visibleWidth = qMax(minimumVisible, result.visibleWidth & ~1);
        result.visibleHeight = qMax(minimumVisible, result.visibleHeight & ~1);
        // Absorb an odd rounding pixel on the far edge so the visible origin stays exact.
        result.right = size.width() - result.left - result.visibleWidth;
        result.bottom = size.height() - result.top - result.visibleHeight;
    }
    return result;
}

QSize rasterSourceSize(const SceneSource &source,
                       const QHash<QString, QSize> &sourcePixelSizes) {
    const QSize supplied = sourcePixelSizes.value(source.id);
    if (supplied.isValid()) return supplied;
    if (source.type == SceneSourceType::Image) {
        QImageReader reader(source.uri);
        return reader.size();
    }
    return {};
}

int evenCeil(qreal value) {
    return qMax(2, (qCeil(value) + 1) & ~1);
}
} // namespace

QHash<QString, qint64> ExportPipeline::trackStartOffsets(const ProjectInfo &project) {
    return readTrackStartOffsets(project);
}

qint64 ExportPipeline::alignedTimelineDuration(
    const QHash<QString, qint64> &trackDurationsNanoseconds,
    const QHash<QString, qint64> &trackStartOffsetsNanoseconds) {
    qint64 timeline = 0;
    for (auto it = trackDurationsNanoseconds.cbegin();
         it != trackDurationsNanoseconds.cend(); ++it) {
        const qint64 duration = qMax<qint64>(0, it.value());
        const qint64 offset = trackStartOffsetsNanoseconds.value(it.key(), 0);
        qint64 end = duration;
        if (offset > 0) {
            end = duration > std::numeric_limits<qint64>::max() - offset
                ? std::numeric_limits<qint64>::max() : duration + offset;
        } else if (offset < 0) {
            end = offset == std::numeric_limits<qint64>::min() || duration < -offset
                ? 0 : duration + offset;
        }
        timeline = qMax(timeline, end);
    }
    return timeline;
}

ExportPipelineResult ExportPipeline::build(const ProjectInfo &project,
                                           const SceneDocument &scene,
                                           SceneFormat format,
                                           const QString &outputPath,
                                           qint64 durationNanoseconds,
                                           const QHash<QString, QSize> &sourcePixelSizes) {
    const SceneComposition &composition = scene.composition(format);
    if (composition.layers.isEmpty()) return {{}, QStringLiteral("The scene has no layers")};
    const QHash<QString, qint64> offsets = ExportPipeline::trackStartOffsets(project);
    QString branches;
    QString declarations;
    int pad = 0;
    for (const SceneLayer &layer : composition.layers) {
        if (!layer.visible || layer.transform.opacity <= 0) continue;
        const SceneSource *source = scene.source(layer.sourceId);
        if (!source) continue;
        const QRectF frame = layer.transform.frame;
        const QMarginsF crop = normalizedSceneCrop(layer.transform.crop);
        const int frameWidth = qMax(2, static_cast<int>(frame.width()) & ~1);
        const int frameHeight = qMax(2, static_cast<int>(frame.height()) & ~1);
        const PixelCrop destinationCrop = cropPixels(
            QSize(frameWidth, frameHeight), crop, 2, true);
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
            sourcePipeline = videoSource(project.airplayDirectory(), QStringLiteral("video"),
                                         offsets.value(QStringLiteral("airplay-video")));
        } else if (source->type == SceneSourceType::Camera &&
                   hasTrack(project.presenterDirectory(), QStringLiteral("camera"), QStringLiteral("mkv"))) {
            sourcePipeline = videoSource(project.presenterDirectory(), QStringLiteral("camera"),
                                         offsets.value(QStringLiteral("camera")));
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

        QString sourceCropStage;
        const bool cropsRasterPixels = source->type == SceneSourceType::AirPlay ||
                                       source->type == SceneSourceType::Camera ||
                                       source->type == SceneSourceType::Image;
        if (cropsRasterPixels && hasCrop(crop)) {
            const QSize sourceSize = rasterSourceSize(*source, sourcePixelSizes);
            if (sourceSize.width() < 2 || sourceSize.height() < 2) {
                return {{}, QStringLiteral("The source dimensions for cropped layer '%1' are unavailable")
                                .arg(layer.name)};
            }
            const PixelCrop sourceCrop = cropPixels(sourceSize, crop, 1, false);
            sourceCropStage = QStringLiteral(
                " ! videocrop left=%1 right=%2 top=%3 bottom=%4")
                .arg(sourceCrop.left).arg(sourceCrop.right)
                .arg(sourceCrop.top).arg(sourceCrop.bottom);
        }

        QString effects;
        const bool rotated = !qFuzzyIsNull(layer.transform.rotationDegrees);
        if (layer.transform.mask != SceneMask::None || rotated) {
            effects += QStringLiteral(" ! videoconvert ! video/x-raw,format=BGRA");
        }
        if (layer.transform.mask != SceneMask::None) {
            effects += QStringLiteral(" ! cairooverlay name=%1-mask-%2")
                .arg(layer.transform.mask == SceneMask::Circle ? QStringLiteral("circle")
                                                               : QStringLiteral("rounded"))
                .arg(pad);
        }

        int compositorX = qRound(frame.x()) + destinationCrop.left;
        int compositorY = qRound(frame.y()) + destinationCrop.top;
        if (rotated) {
            const qreal radians = qDegreesToRadians(layer.transform.rotationDegrees);
            const int rotatedWidth = evenCeil(qAbs(frameWidth * qCos(radians)) +
                                              qAbs(frameHeight * qSin(radians)));
            const int rotatedHeight = evenCeil(qAbs(frameWidth * qSin(radians)) +
                                               qAbs(frameHeight * qCos(radians)));
            const int horizontalPadding = (rotatedWidth - frameWidth) / 2;
            const int verticalPadding = (rotatedHeight - frameHeight) / 2;
            effects += QStringLiteral(
                " ! videobox left=-%1 right=-%2 top=-%3 bottom=-%4 border-alpha=0 "
                "! video/x-raw,format=BGRA,width=%5,height=%6 ! rotate angle=%7")
                .arg(destinationCrop.left + horizontalPadding)
                .arg(destinationCrop.right + horizontalPadding)
                .arg(destinationCrop.top + verticalPadding)
                .arg(destinationCrop.bottom + verticalPadding)
                .arg(rotatedWidth).arg(rotatedHeight)
                .arg(radians, 0, 'f', 8);
            compositorX = qRound(frame.x()) - horizontalPadding;
            compositorY = qRound(frame.y()) - verticalPadding;
        }

        branches += QStringLiteral(
            "%1 ! queue ! videoconvert%2 ! videoscale ! video/x-raw,width=%3,height=%4%5 "
            "! comp.sink_%6 ")
            .arg(sourcePipeline, sourceCropStage)
            .arg(destinationCrop.visibleWidth).arg(destinationCrop.visibleHeight)
            .arg(effects).arg(pad);
        declarations += QStringLiteral(
            "sink_%1::xpos=%2 sink_%1::ypos=%3 sink_%1::alpha=%4 ")
            .arg(pad).arg(compositorX).arg(compositorY)
            .arg(layer.transform.opacity, 0, 'f', 3);
        ++pad;
    }
    if (pad == 0) return {{}, QStringLiteral("No recorded or supported visible layers are available")};

    const QSize size = composition.canvasSize;
    QString pipeline = branches + QStringLiteral(
        "compositor name=comp background=black %1 ! videoconvert "
        "! video/x-raw,format=NV12,width=%2,height=%3,framerate=60/1 "
        "! mfh264enc bitrate=12000 low-latency=false rc-mode=qvbr qp=22 quality-vs-speed=65 "
        "! h264parse ! queue ! mux.video_0 mp4mux name=mux faststart=true "
        "! filesink location=%4 ")
        .arg(declarations).arg(size.width()).arg(size.height()).arg(quote(outputPath));

    QStringList audio;
    if (hasTrack(project.airplayDirectory(), QStringLiteral("audio"), QStringLiteral("mka"))) {
        audio << QStringLiteral(
            "splitmuxsrc location=%1 ! decodebin ! identity ts-offset=%2 "
            "! audioconvert ! audioresample ! mix.")
            .arg(quote(firstPattern(project.airplayDirectory(), QStringLiteral("audio"),
                                    QStringLiteral("mka"))))
            .arg(offsets.value(QStringLiteral("airplay-audio")));
    }
    if (hasTrack(project.presenterDirectory(), QStringLiteral("microphone"), QStringLiteral("mka"))) {
        audio << QStringLiteral(
            "splitmuxsrc location=%1 ! decodebin ! identity ts-offset=%2 "
            "! audioconvert ! audioresample ! mix.")
            .arg(quote(firstPattern(project.presenterDirectory(), QStringLiteral("microphone"),
                                    QStringLiteral("mka"))))
            .arg(offsets.value(QStringLiteral("microphone")));
    }
    if (!audio.isEmpty()) {
        pipeline += audio.join(QLatin1Char(' ')) + QStringLiteral(
            " audiomixer name=mix ! avenc_aac bitrate=192000 ! aacparse ! queue ! mux.audio_0");
    }
    return {pipeline, {}};
}
