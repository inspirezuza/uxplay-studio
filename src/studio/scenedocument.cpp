#include "scenedocument.h"

#include <QJsonArray>
#include <QUuid>
#include <algorithm>

namespace {

QString newId(const char *prefix) {
    return QString::fromLatin1(prefix) + QStringLiteral("-") +
           QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QJsonObject rectToJson(const QRectF &rect) {
    return {{QStringLiteral("x"), rect.x()}, {QStringLiteral("y"), rect.y()},
            {QStringLiteral("width"), rect.width()},
            {QStringLiteral("height"), rect.height()}};
}

QRectF rectFromJson(const QJsonObject &json) {
    return {json.value(QStringLiteral("x")).toDouble(),
            json.value(QStringLiteral("y")).toDouble(),
            json.value(QStringLiteral("width")).toDouble(),
            json.value(QStringLiteral("height")).toDouble()};
}

QJsonObject marginsToJson(const QMarginsF &margins) {
    return {{QStringLiteral("left"), margins.left()},
            {QStringLiteral("top"), margins.top()},
            {QStringLiteral("right"), margins.right()},
            {QStringLiteral("bottom"), margins.bottom()}};
}

QMarginsF marginsFromJson(const QJsonObject &json) {
    return {json.value(QStringLiteral("left")).toDouble(),
            json.value(QStringLiteral("top")).toDouble(),
            json.value(QStringLiteral("right")).toDouble(),
            json.value(QStringLiteral("bottom")).toDouble()};
}

SceneSourceType sourceTypeFromKey(const QString &key) {
    if (key == QStringLiteral("camera")) return SceneSourceType::Camera;
    if (key == QStringLiteral("image")) return SceneSourceType::Image;
    if (key == QStringLiteral("text")) return SceneSourceType::Text;
    if (key == QStringLiteral("color")) return SceneSourceType::Color;
    return SceneSourceType::AirPlay;
}

SceneMask maskFromKey(const QString &key) {
    if (key == QStringLiteral("roundedRectangle")) return SceneMask::RoundedRectangle;
    if (key == QStringLiteral("circle")) return SceneMask::Circle;
    return SceneMask::None;
}

QJsonObject layerToJson(const SceneLayer &layer) {
    return {{QStringLiteral("id"), layer.id},
            {QStringLiteral("sourceId"), layer.sourceId},
            {QStringLiteral("name"), layer.name},
            {QStringLiteral("visible"), layer.visible},
            {QStringLiteral("locked"), layer.locked},
            {QStringLiteral("transform"),
             QJsonObject{{QStringLiteral("frame"), rectToJson(layer.transform.frame)},
                         {QStringLiteral("crop"), marginsToJson(layer.transform.crop)},
                         {QStringLiteral("rotationDegrees"), layer.transform.rotationDegrees},
                         {QStringLiteral("opacity"), layer.transform.opacity},
                         {QStringLiteral("mask"), sceneMaskKey(layer.transform.mask)}}}};
}

SceneLayer layerFromJson(const QJsonObject &json) {
    SceneLayer layer;
    layer.id = json.value(QStringLiteral("id")).toString();
    layer.sourceId = json.value(QStringLiteral("sourceId")).toString();
    layer.name = json.value(QStringLiteral("name")).toString();
    layer.visible = json.value(QStringLiteral("visible")).toBool(true);
    layer.locked = json.value(QStringLiteral("locked")).toBool(false);
    const auto transform = json.value(QStringLiteral("transform")).toObject();
    layer.transform.frame = rectFromJson(transform.value(QStringLiteral("frame")).toObject());
    layer.transform.crop = marginsFromJson(transform.value(QStringLiteral("crop")).toObject());
    layer.transform.rotationDegrees = transform.value(QStringLiteral("rotationDegrees")).toDouble();
    layer.transform.opacity = transform.value(QStringLiteral("opacity")).toDouble(1.0);
    layer.transform.mask = maskFromKey(transform.value(QStringLiteral("mask")).toString());
    return layer;
}

QJsonObject compositionToJson(const SceneComposition &composition) {
    QJsonArray layers;
    for (const SceneLayer &layer : composition.layers) layers.append(layerToJson(layer));
    return {{QStringLiteral("width"), composition.canvasSize.width()},
            {QStringLiteral("height"), composition.canvasSize.height()},
            {QStringLiteral("layers"), layers}};
}

SceneComposition compositionFromJson(const QJsonObject &json, const QSize &fallbackSize) {
    SceneComposition composition;
    composition.canvasSize = QSize(json.value(QStringLiteral("width")).toInt(fallbackSize.width()),
                                   json.value(QStringLiteral("height")).toInt(fallbackSize.height()));
    for (const auto &value : json.value(QStringLiteral("layers")).toArray()) {
        composition.layers.append(layerFromJson(value.toObject()));
    }
    return composition;
}

} // namespace

SceneDocument::SceneDocument() {
    m_wide.canvasSize = QSize(1920, 1080);
    m_vertical.canvasSize = QSize(1080, 1920);
}

QString SceneDocument::title() const { return m_title; }
void SceneDocument::setTitle(const QString &title) { m_title = title.trimmed(); }
const QList<SceneSource> &SceneDocument::sources() const { return m_sources; }

const SceneSource *SceneDocument::source(const QString &id) const {
    const auto it = std::find_if(m_sources.cbegin(), m_sources.cend(),
                                 [&id](const SceneSource &item) { return item.id == id; });
    return it == m_sources.cend() ? nullptr : &*it;
}

QString SceneDocument::addSource(SceneSourceType type, const QString &name, const QString &uri) {
    SceneSource item;
    item.id = newId("source");
    item.type = type;
    item.name = name.trimmed().isEmpty() ? QStringLiteral("Source") : name.trimmed();
    item.uri = uri;
    m_sources.append(item);
    return item.id;
}

bool SceneDocument::removeSource(const QString &id) {
    const int removed = m_sources.removeIf([&id](const SceneSource &item) { return item.id == id; });
    if (!removed) return false;
    m_wide.layers.removeIf([&id](const SceneLayer &layer) { return layer.sourceId == id; });
    m_vertical.layers.removeIf([&id](const SceneLayer &layer) { return layer.sourceId == id; });
    return true;
}

const SceneComposition &SceneDocument::composition(SceneFormat format) const {
    return format == SceneFormat::Wide ? m_wide : m_vertical;
}

SceneComposition &SceneDocument::mutableComposition(SceneFormat format) {
    return format == SceneFormat::Wide ? m_wide : m_vertical;
}

const SceneLayer *SceneDocument::layer(SceneFormat format, const QString &id) const {
    const auto &layers = composition(format).layers;
    const auto it = std::find_if(layers.cbegin(), layers.cend(),
                                 [&id](const SceneLayer &item) { return item.id == id; });
    return it == layers.cend() ? nullptr : &*it;
}

SceneLayer *SceneDocument::mutableLayer(SceneFormat format, const QString &id) {
    auto &layers = mutableComposition(format).layers;
    const auto it = std::find_if(layers.begin(), layers.end(),
                                 [&id](const SceneLayer &item) { return item.id == id; });
    return it == layers.end() ? nullptr : &*it;
}

QString SceneDocument::addLayer(SceneFormat format, const QString &sourceId) {
    const SceneSource *item = source(sourceId);
    if (!item) return {};
    SceneLayer layer;
    layer.id = newId("layer");
    layer.sourceId = sourceId;
    layer.name = item->name;
    const QSize size = composition(format).canvasSize;
    layer.transform.frame = QRectF(size.width() * 0.1, size.height() * 0.1,
                                   size.width() * 0.8, size.height() * 0.8);
    mutableComposition(format).layers.append(layer);
    return layer.id;
}

bool SceneDocument::removeLayer(SceneFormat format, const QString &layerId) {
    return mutableComposition(format).layers.removeIf(
               [&layerId](const SceneLayer &layer) { return layer.id == layerId; }) > 0;
}

bool SceneDocument::moveLayer(SceneFormat format, const QString &layerId, int newIndex) {
    auto &layers = mutableComposition(format).layers;
    const auto it = std::find_if(layers.cbegin(), layers.cend(),
                                 [&layerId](const SceneLayer &layer) { return layer.id == layerId; });
    if (it == layers.cend() || newIndex < 0 || newIndex >= layers.size()) return false;
    const int oldIndex = static_cast<int>(std::distance(layers.cbegin(), it));
    if (oldIndex == newIndex) return true;
    layers.move(oldIndex, newIndex);
    return true;
}

bool SceneDocument::setLayerVisible(SceneFormat format, const QString &layerId, bool visible) {
    SceneLayer *item = mutableLayer(format, layerId);
    if (!item) return false;
    item->visible = visible;
    return true;
}

bool SceneDocument::setLayerLocked(SceneFormat format, const QString &layerId, bool locked) {
    SceneLayer *item = mutableLayer(format, layerId);
    if (!item) return false;
    item->locked = locked;
    return true;
}

bool SceneDocument::setTransform(SceneFormat format, const QString &layerId,
                                 const SceneTransform &transform) {
    SceneLayer *item = mutableLayer(format, layerId);
    if (!item || transform.frame.width() <= 0.0 || transform.frame.height() <= 0.0 ||
        transform.opacity < 0.0 || transform.opacity > 1.0) return false;
    item->transform = transform;
    return true;
}

QString SceneDocument::duplicateLayer(SceneFormat format, const QString &layerId) {
    const SceneLayer *original = layer(format, layerId);
    if (!original) return {};
    SceneLayer copy = *original;
    copy.id = newId("layer");
    copy.name += QStringLiteral(" copy");
    copy.transform.frame.translate(24, 24);
    mutableComposition(format).layers.append(copy);
    return copy.id;
}

QJsonObject SceneDocument::toJson() const {
    QJsonArray sources;
    for (const SceneSource &item : m_sources) {
        sources.append(QJsonObject{{QStringLiteral("id"), item.id},
                                   {QStringLiteral("type"), sceneSourceTypeKey(item.type)},
                                   {QStringLiteral("name"), item.name},
                                   {QStringLiteral("uri"), item.uri}});
    }
    return {{QStringLiteral("schemaVersion"), 1},
            {QStringLiteral("title"), m_title},
            {QStringLiteral("sources"), sources},
            {QStringLiteral("compositions"),
             QJsonObject{{QStringLiteral("wide"), compositionToJson(m_wide)},
                         {QStringLiteral("vertical"), compositionToJson(m_vertical)}}}};
}

SceneDocument::LoadResult SceneDocument::fromJson(const QJsonObject &json) {
    if (json.value(QStringLiteral("schemaVersion")).toInt() != 1) {
        return {nullptr, QStringLiteral("Unsupported project schema version")};
    }
    auto document = std::make_unique<SceneDocument>();
    document->m_title = json.value(QStringLiteral("title")).toString(QStringLiteral("Untitled project"));
    for (const auto &value : json.value(QStringLiteral("sources")).toArray()) {
        const auto sourceJson = value.toObject();
        SceneSource item;
        item.id = sourceJson.value(QStringLiteral("id")).toString();
        item.type = sourceTypeFromKey(sourceJson.value(QStringLiteral("type")).toString());
        item.name = sourceJson.value(QStringLiteral("name")).toString();
        item.uri = sourceJson.value(QStringLiteral("uri")).toString();
        if (item.id.isEmpty()) return {nullptr, QStringLiteral("A source is missing its id")};
        document->m_sources.append(item);
    }
    const auto compositions = json.value(QStringLiteral("compositions")).toObject();
    document->m_wide = compositionFromJson(compositions.value(QStringLiteral("wide")).toObject(),
                                           QSize(1920, 1080));
    document->m_vertical = compositionFromJson(
        compositions.value(QStringLiteral("vertical")).toObject(), QSize(1080, 1920));
    return {std::move(document), {}};
}

QString sceneFormatKey(SceneFormat format) {
    return format == SceneFormat::Wide ? QStringLiteral("wide") : QStringLiteral("vertical");
}

QString sceneSourceTypeKey(SceneSourceType type) {
    switch (type) {
    case SceneSourceType::AirPlay: return QStringLiteral("airplay");
    case SceneSourceType::Camera: return QStringLiteral("camera");
    case SceneSourceType::Image: return QStringLiteral("image");
    case SceneSourceType::Text: return QStringLiteral("text");
    case SceneSourceType::Color: return QStringLiteral("color");
    }
    return QStringLiteral("airplay");
}

QString sceneMaskKey(SceneMask mask) {
    switch (mask) {
    case SceneMask::None: return QStringLiteral("none");
    case SceneMask::RoundedRectangle: return QStringLiteral("roundedRectangle");
    case SceneMask::Circle: return QStringLiteral("circle");
    }
    return QStringLiteral("none");
}
