#pragma once

#include <QJsonObject>
#include <QList>
#include <QMarginsF>
#include <QRectF>
#include <QSize>
#include <QString>

#include <memory>

enum class SceneFormat {
    Wide,
    Vertical
};

enum class SceneSourceType {
    AirPlay,
    Camera,
    Image,
    Text,
    Color
};

enum class SceneMask {
    None,
    RoundedRectangle,
    Circle
};

struct SceneSource {
    QString id;
    SceneSourceType type = SceneSourceType::AirPlay;
    QString name;
    QString uri;
};

struct SceneTransform {
    QRectF frame;
    QMarginsF crop;
    qreal rotationDegrees = 0.0;
    qreal opacity = 1.0;
    SceneMask mask = SceneMask::None;

    bool operator==(const SceneTransform &other) const
    {
        return frame == other.frame && crop == other.crop
            && qFuzzyCompare(1.0 + rotationDegrees, 1.0 + other.rotationDegrees)
            && qFuzzyCompare(1.0 + opacity, 1.0 + other.opacity)
            && mask == other.mask;
    }

    bool operator!=(const SceneTransform &other) const { return !(*this == other); }
};

struct SceneLayer {
    QString id;
    QString sourceId;
    QString name;
    bool visible = true;
    bool locked = false;
    SceneTransform transform;
};

struct SceneComposition {
    QSize canvasSize;
    QList<SceneLayer> layers;
};

class SceneDocument final {
public:
    struct LoadResult {
        std::unique_ptr<SceneDocument> document;
        QString error;
        bool has_value() const { return document != nullptr; }
    };

    SceneDocument();

    QString title() const;
    void setTitle(const QString &title);

    const QList<SceneSource> &sources() const;
    const SceneSource *source(const QString &id) const;
    QString addSource(SceneSourceType type, const QString &name, const QString &uri = {});
    bool removeSource(const QString &id);

    const SceneComposition &composition(SceneFormat format) const;
    const SceneLayer *layer(SceneFormat format, const QString &id) const;
    QString addLayer(SceneFormat format, const QString &sourceId);
    bool removeLayer(SceneFormat format, const QString &layerId);
    bool moveLayer(SceneFormat format, const QString &layerId, int newIndex);
    bool setLayerVisible(SceneFormat format, const QString &layerId, bool visible);
    bool setLayerLocked(SceneFormat format, const QString &layerId, bool locked);
    bool setTransform(SceneFormat format, const QString &layerId,
                      const SceneTransform &transform);
    QString duplicateLayer(SceneFormat format, const QString &layerId);

    QJsonObject toJson() const;
    static LoadResult fromJson(const QJsonObject &json);

private:
    SceneComposition &mutableComposition(SceneFormat format);
    SceneLayer *mutableLayer(SceneFormat format, const QString &id);

    QString m_title = QStringLiteral("Untitled project");
    QList<SceneSource> m_sources;
    SceneComposition m_wide;
    SceneComposition m_vertical;
};

QString sceneFormatKey(SceneFormat format);
QString sceneSourceTypeKey(SceneSourceType type);
QString sceneMaskKey(SceneMask mask);
QMarginsF normalizedSceneCrop(const QMarginsF &crop);
