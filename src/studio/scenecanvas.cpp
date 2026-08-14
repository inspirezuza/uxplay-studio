#include "scenecanvas.h"

#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QImage>
#include <QResizeEvent>
#include <QUndoCommand>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {

constexpr int EdgeLeft = 1;
constexpr int EdgeTop = 2;
constexpr int EdgeRight = 4;
constexpr int EdgeBottom = 8;

class TransformCommand final : public QUndoCommand {
public:
    TransformCommand(SceneDocument *document, SceneFormat format,
                     QHash<QString, SceneTransform> before,
                     QHash<QString, SceneTransform> after,
                     std::function<void()> refresh, const QString &label)
        : QUndoCommand(label), m_document(document), m_format(format),
          m_before(std::move(before)), m_after(std::move(after)),
          m_refresh(std::move(refresh)) {}

    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }

private:
    void apply(const QHash<QString, SceneTransform> &values) {
        if (!m_document) return;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            m_document->setTransform(m_format, it.key(), it.value());
        }
        m_refresh();
    }

    SceneDocument *m_document;
    SceneFormat m_format;
    QHash<QString, SceneTransform> m_before;
    QHash<QString, SceneTransform> m_after;
    std::function<void()> m_refresh;
};

QColor sourceColor(SceneSourceType type) {
    switch (type) {
    case SceneSourceType::AirPlay: return QColor(QStringLiteral("#253a67"));
    case SceneSourceType::Camera: return QColor(QStringLiteral("#554373"));
    case SceneSourceType::Image: return QColor(QStringLiteral("#345d60"));
    case SceneSourceType::Text: return QColor(QStringLiteral("#5c4b31"));
    case SceneSourceType::Color: return QColor(QStringLiteral("#3b414e"));
    }
    return QColor(QStringLiteral("#253a67"));
}

} // namespace

class SceneCanvas::LayerItem final : public QGraphicsRectItem {
public:
    LayerItem(QString layerId, SceneSource source)
        : m_layerId(std::move(layerId)), m_source(std::move(source)) {
        setFlag(QGraphicsItem::ItemIsSelectable, true);
        setAcceptHoverEvents(true);
    }

    QString layerId() const { return m_layerId; }
    QString sourceId() const { return m_source.id; }
    void setPreview(const QImage &preview) { m_preview = preview; update(); }

    QRectF boundingRect() const override {
        // The rotate handle is kept a stable distance from the top resize
        // handle in screen pixels. At zoom-out that requires more scene-space
        // room than the layer rectangle itself.
        return QGraphicsRectItem::boundingRect().adjusted(-18, -180, 18, 18);
    }

    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option,
               QWidget *widget) override {
        Q_UNUSED(option)
        Q_UNUSED(widget)
        painter->save();
        const QRectF content = rect();
        const qreal left = content.width() * m_crop.left();
        const qreal top = content.height() * m_crop.top();
        const qreal right = content.width() * m_crop.right();
        const qreal bottom = content.height() * m_crop.bottom();
        const QRectF visible = content.adjusted(left, top, -right, -bottom);
        QPainterPath clip;
        if (m_mask == SceneMask::Circle) clip.addEllipse(visible);
        else if (m_mask == SceneMask::RoundedRectangle) clip.addRoundedRect(visible, 36, 36);
        else clip.addRect(visible);
        painter->setClipPath(clip);
        QColor fill = sourceColor(m_source.type);
        if (m_source.type == SceneSourceType::Color && QColor::isValidColorName(m_source.uri))
            fill = QColor(m_source.uri);
        if (m_source.type == SceneSourceType::Text) fill = Qt::black;
        painter->fillRect(visible, fill);
        if (m_source.type == SceneSourceType::Image) {
            const QImage image(m_source.uri);
            if (!image.isNull()) {
                const QRectF sourceRect(image.width() * m_crop.left(),
                                        image.height() * m_crop.top(),
                                        image.width() * (1.0 - m_crop.left() - m_crop.right()),
                                        image.height() * (1.0 - m_crop.top() - m_crop.bottom()));
                if (sourceRect.isValid()) painter->drawImage(visible, image, sourceRect);
            }
        }
        if ((m_source.type == SceneSourceType::AirPlay || m_source.type == SceneSourceType::Camera) &&
            !m_preview.isNull()) {
            const QRectF sourceRect(m_preview.width() * m_crop.left(),
                                    m_preview.height() * m_crop.top(),
                                    m_preview.width() * (1.0 - m_crop.left() - m_crop.right()),
                                    m_preview.height() * (1.0 - m_crop.top() - m_crop.bottom()));
            if (sourceRect.isValid()) painter->drawImage(visible, m_preview, sourceRect);
        }
        if ((m_source.type == SceneSourceType::AirPlay || m_source.type == SceneSourceType::Camera) &&
            m_preview.isNull()) {
            QLinearGradient gradient(visible.topLeft(), visible.bottomRight());
            gradient.setColorAt(0, QColor(255, 255, 255, 28));
            gradient.setColorAt(1, QColor(0, 0, 0, 38));
            painter->fillRect(visible, gradient);
        }
        painter->setPen(QColor(QStringLiteral("#f4f7ff")));
        QFont font = painter->font();
        font.setPixelSize(qBound(16, static_cast<int>(content.height() / 12), 44));
        font.setWeight(QFont::DemiBold);
        painter->setFont(font);
        if (((m_source.type == SceneSourceType::AirPlay || m_source.type == SceneSourceType::Camera) &&
             m_preview.isNull()) || m_source.type == SceneSourceType::Text)
            painter->drawText(visible.adjusted(18, 18, -18, -18),
                              Qt::AlignCenter | Qt::TextWordWrap, m_source.name);
        painter->restore();

        if (isSelected()) {
            painter->save();
            QPen pen(QColor(QStringLiteral("#63a2ff")), 3);
            pen.setCosmetic(true);
            painter->setPen(pen);
            painter->setBrush(Qt::NoBrush);
            painter->drawRect(content);
            painter->setBrush(QColor(QStringLiteral("#f8fbff")));
            const qreal viewScale = qMax<qreal>(0.05,
                std::hypot(painter->transform().m11(), painter->transform().m12()));
            const qreal handle = 10.0 / viewScale;
            const QList<QPointF> points{content.topLeft(), content.topRight(),
                                       content.bottomLeft(), content.bottomRight(),
                                       QPointF(content.center().x(), content.top()),
                                       QPointF(content.center().x(), content.bottom()),
                                       QPointF(content.left(), content.center().y()),
                                       QPointF(content.right(), content.center().y())};
            for (const QPointF &point : points) {
                painter->drawRect(QRectF(point.x() - handle / 2, point.y() - handle / 2,
                                         handle, handle));
            }
            const QPointF rotatePoint(content.center().x(), content.top() - 28.0 / viewScale);
            painter->drawLine(QPointF(content.center().x(), content.top()), rotatePoint);
            painter->drawEllipse(rotatePoint, handle / 2, handle / 2);
            painter->restore();
        }
    }

    void sync(const SceneLayer &layer, int z) {
        prepareGeometryChange();
        setRect(0, 0, layer.transform.frame.width(), layer.transform.frame.height());
        setPos(layer.transform.frame.topLeft());
        setTransformOriginPoint(rect().center());
        setRotation(layer.transform.rotationDegrees);
        setOpacity(layer.transform.opacity);
        setVisible(layer.visible);
        setEnabled(!layer.locked);
        setZValue(z);
        m_crop = layer.transform.crop;
        m_mask = layer.transform.mask;
        update();
    }

private:
    QString m_layerId;
    SceneSource m_source;
    QImage m_preview;
    QMarginsF m_crop;
    SceneMask m_mask = SceneMask::None;
};

SceneCanvas::SceneCanvas(QWidget *parent) : QGraphicsView(parent) {
    m_scene = new QGraphicsScene(this);
    setScene(m_scene);
    setObjectName(QStringLiteral("sceneCanvas"));
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform |
                   QPainter::TextAntialiasing);
    setBackgroundBrush(QColor(QStringLiteral("#090c12")));
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::RubberBandDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &SceneCanvas::emitSelection);
}

SceneCanvas::~SceneCanvas() {
    m_undoStack.clear();
    if (m_scene) {
        disconnect(m_scene, nullptr, this, nullptr);
        setScene(nullptr);
        delete m_scene;
        m_scene = nullptr;
    }
}

void SceneCanvas::setDocument(SceneDocument *document, SceneFormat format) {
    m_document = document;
    m_format = format;
    m_undoStack.clear();
    rebuild();
}

SceneDocument *SceneCanvas::document() const { return m_document; }
SceneFormat SceneCanvas::format() const { return m_format; }

void SceneCanvas::setPresentationMode(bool enabled) {
    if (m_presentationMode == enabled) return;
    m_presentationMode = enabled;
    if (enabled) {
        m_interaction = Interaction::None;
        m_activeLayer.clear();
        m_beforeInteraction.clear();
        clearLayerSelection();
        unsetCursor();
        fitCanvas();
    }
    viewport()->setCursor(enabled ? Qt::BlankCursor : Qt::ArrowCursor);
    viewport()->update();
}

bool SceneCanvas::presentationMode() const { return m_presentationMode; }

void SceneCanvas::setEditingEnabled(bool enabled) {
    if (m_editingEnabled == enabled) return;
    m_editingEnabled = enabled;
    if (!enabled) {
        m_interaction = Interaction::None;
        m_activeLayer.clear();
        m_beforeInteraction.clear();
        unsetCursor();
    }
    viewport()->update();
}

bool SceneCanvas::editingEnabled() const { return m_editingEnabled; }

void SceneCanvas::setFormat(SceneFormat format) {
    if (m_format == format) return;
    m_format = format;
    rebuild();
}

QSize SceneCanvas::canvasSize() const {
    return m_document ? m_document->composition(m_format).canvasSize : QSize();
}

void SceneCanvas::fitCanvas() {
    if (!m_scene || m_scene->sceneRect().isEmpty()) return;
    resetTransform();
    fitInView(m_scene->sceneRect(), Qt::KeepAspectRatio);
    m_viewScale = 1.0;
    m_autoFit = true;
    emitZoomChanged();
}

void SceneCanvas::zoomIn() {
    zoomBy(1.15);
}

void SceneCanvas::zoomOut() {
    zoomBy(1.0 / 1.15);
}

int SceneCanvas::zoomPercent() const {
    return qRound(m_viewScale * 100.0);
}

void SceneCanvas::zoomBy(qreal factor, const QPoint *viewAnchor) {
    if (!m_scene || m_scene->sceneRect().isEmpty() || factor <= 0.0) return;
    const qreal nextScale = qBound<qreal>(0.2, m_viewScale * factor, 4.0);
    if (qFuzzyCompare(nextScale, m_viewScale)) return;
    const qreal ratio = nextScale / m_viewScale;
    const auto previousAnchor = transformationAnchor();
    setTransformationAnchor(viewAnchor ? QGraphicsView::AnchorUnderMouse
                                        : QGraphicsView::AnchorViewCenter);
    m_autoFit = false;
    scale(ratio, ratio);
    setTransformationAnchor(previousAnchor);
    m_viewScale = nextScale;
    emitZoomChanged();
}

void SceneCanvas::emitZoomChanged() {
    emit zoomChanged(zoomPercent());
}

bool SceneCanvas::selectLayer(const QString &layerId, bool add) {
    LayerItem *item = m_items.value(layerId);
    if (!item) return false;
    if (!add) m_scene->clearSelection();
    item->setSelected(true);
    ensureVisible(item, 32, 32);
    return true;
}

QStringList SceneCanvas::selectedLayerIds() const {
    QStringList ids;
    for (QGraphicsItem *item : m_scene->selectedItems()) {
        if (auto *layer = dynamic_cast<LayerItem *>(item)) ids.append(layer->layerId());
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

void SceneCanvas::clearLayerSelection() { m_scene->clearSelection(); }

QHash<QString, SceneTransform> SceneCanvas::selectedTransforms() const {
    QHash<QString, SceneTransform> values;
    if (!m_document) return values;
    for (const QString &id : selectedLayerIds()) {
        if (const SceneLayer *item = m_document->layer(m_format, id); item && !item->locked) {
            values.insert(id, item->transform);
        }
    }
    return values;
}

void SceneCanvas::pushTransforms(const QString &label,
                                 const QHash<QString, SceneTransform> &before,
                                 const QHash<QString, SceneTransform> &after) {
    if (!m_document || before.isEmpty() || before == after) return;
    m_undoStack.push(new TransformCommand(
        m_document, m_format, before, after,
        [this]() { syncItemsFromDocument(); emit sceneChanged(); }, label));
}

void SceneCanvas::nudgeSelection(const QPointF &delta) {
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it) it->frame.translate(delta);
    pushTransforms(QStringLiteral("Move layers"), before, after);
}

void SceneCanvas::fitSelection() {
    const auto before = selectedTransforms();
    auto after = before;
    const QSize size = canvasSize();
    for (auto it = after.begin(); it != after.end(); ++it) it->frame = QRectF(QPointF(0, 0), size);
    pushTransforms(QStringLiteral("Fit layers to canvas"), before, after);
}

void SceneCanvas::centerSelection() {
    const auto before = selectedTransforms();
    auto after = before;
    const QPointF center(canvasSize().width() / 2.0, canvasSize().height() / 2.0);
    for (auto it = after.begin(); it != after.end(); ++it) it->frame.moveCenter(center);
    pushTransforms(QStringLiteral("Center layers"), before, after);
}

void SceneCanvas::resetSelection() {
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it) {
        it->crop = {};
        it->rotationDegrees = 0.0;
        it->opacity = 1.0;
        it->mask = SceneMask::None;
    }
    pushTransforms(QStringLiteral("Reset layer transforms"), before, after);
}

void SceneCanvas::rotateSelection(qreal deltaDegrees) {
    if (qFuzzyIsNull(deltaDegrees)) return;
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it)
        it->rotationDegrees += deltaDegrees;
    pushTransforms(QStringLiteral("Rotate layers"), before, after);
}

void SceneCanvas::cropSelection(const QMarginsF &crop) {
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it)
        it->crop = normalizedSceneCrop(crop);
    pushTransforms(QStringLiteral("Crop layers"), before, after);
}

void SceneCanvas::setSelectionOpacity(qreal opacity) {
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it) it->opacity = qBound<qreal>(0, opacity, 1);
    pushTransforms(QStringLiteral("Change layer opacity"), before, after);
}

void SceneCanvas::setSelectionMask(SceneMask mask) {
    const auto before = selectedTransforms();
    auto after = before;
    for (auto it = after.begin(); it != after.end(); ++it) it->mask = mask;
    pushTransforms(QStringLiteral("Change layer mask"), before, after);
}

void SceneCanvas::setSelectionGeometry(const QRectF &frame, qreal rotationDegrees) {
    if (!frame.isValid() || frame.width() < 1.0 || frame.height() < 1.0) return;
    const auto before = selectedTransforms();
    if (before.size() != 1) return;
    auto after = before;
    auto it = after.begin();
    it.value().frame = frame;
    it.value().rotationDegrees = rotationDegrees;
    pushTransforms(QStringLiteral("Edit layer geometry"), before, after);
}

void SceneCanvas::setSnapEnabled(bool enabled) { m_snapEnabled = enabled; }
bool SceneCanvas::snapEnabled() const { return m_snapEnabled; }
QUndoStack *SceneCanvas::undoStack() { return &m_undoStack; }
void SceneCanvas::refreshFromDocument() { syncItemsFromDocument(); }

void SceneCanvas::setSourcePreview(const QString &sourceId, const QImage &frame) {
    if (sourceId.isEmpty() || frame.isNull()) return;
    m_sourcePreviews.insert(sourceId, frame);
    for (LayerItem *item : m_items) {
        if (item->sourceId() == sourceId) item->setPreview(frame);
    }
}

bool SceneCanvas::deleteSelection() {
    if (!m_document) return false;
    bool removed = false;
    for (const QString &id : selectedLayerIds()) {
        const SceneLayer *layer = m_document->layer(m_format, id);
        if (layer && !layer->locked) {
            m_document->removeLayer(m_format, id);
            removed = true;
        }
    }
    if (!removed) return false;
    rebuild();
    emit layersChanged();
    emit sceneChanged();
    return true;
}

void SceneCanvas::rebuild() {
    const QStringList selected = selectedLayerIds();
    m_scene->clear();
    m_items.clear();
    if (!m_document) return;
    const SceneComposition &composition = m_document->composition(m_format);
    m_scene->setSceneRect(QRectF(QPointF(0, 0), composition.canvasSize));
    m_scene->setBackgroundBrush(QColor(QStringLiteral("#11151d")));
    for (int i = 0; i < composition.layers.size(); ++i) {
        const SceneLayer &layer = composition.layers.at(i);
        const SceneSource *source = m_document->source(layer.sourceId);
        if (!source) continue;
        auto *item = new LayerItem(layer.id, *source);
        if (m_sourcePreviews.contains(source->id)) item->setPreview(m_sourcePreviews.value(source->id));
        m_scene->addItem(item);
        item->sync(layer, i);
        m_items.insert(layer.id, item);
        if (selected.contains(layer.id)) item->setSelected(true);
    }
    fitCanvas();
    emitSelection();
}

void SceneCanvas::syncItemsFromDocument() {
    if (!m_document) return;
    const auto &layers = m_document->composition(m_format).layers;
    if (layers.size() != m_items.size()) {
        rebuild();
        return;
    }
    for (int i = 0; i < layers.size(); ++i) {
        LayerItem *item = m_items.value(layers.at(i).id);
        if (!item) {
            rebuild();
            return;
        }
        item->sync(layers.at(i), i);
    }
    viewport()->update();
}

void SceneCanvas::applyTransforms(const QHash<QString, SceneTransform> &transforms) {
    if (!m_document) return;
    for (auto it = transforms.cbegin(); it != transforms.cend(); ++it) {
        m_document->setTransform(m_format, it.key(), it.value());
    }
    syncItemsFromDocument();
}

void SceneCanvas::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    if (m_document && m_interaction == Interaction::None && m_autoFit) {
        fitCanvas();
    }
}

void SceneCanvas::wheelEvent(QWheelEvent *event) {
    if (event->modifiers().testFlag(Qt::ControlModifier) && event->angleDelta().y() != 0) {
        const QPoint anchor = event->position().toPoint();
        zoomBy(event->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15, &anchor);
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void SceneCanvas::contextMenuEvent(QContextMenuEvent *event) {
    if (m_presentationMode || !m_editingEnabled) {
        event->ignore();
        return;
    }
    LayerItem *item = layerItemAt(event->pos());
    const QString layerId = item ? item->layerId() : QString();
    if (!layerId.isEmpty() && !selectedLayerIds().contains(layerId))
        selectLayer(layerId);
    emit contextMenuRequested(layerId, event->globalPos());
    event->accept();
}

void SceneCanvas::keyPressEvent(QKeyEvent *event) {
    if (m_presentationMode || !m_editingEnabled) {
        event->ignore();
        return;
    }
    if (event->matches(QKeySequence::Undo)) { m_undoStack.undo(); event->accept(); return; }
    if (event->matches(QKeySequence::Redo)) { m_undoStack.redo(); event->accept(); return; }
    if (event->modifiers().testFlag(Qt::ControlModifier) &&
        (event->key() == Qt::Key_Plus || event->key() == Qt::Key_Equal)) {
        zoomIn();
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier) && event->key() == Qt::Key_Minus) {
        zoomOut();
        event->accept();
        return;
    }
    if (event->modifiers().testFlag(Qt::ControlModifier) && event->key() == Qt::Key_0) {
        fitCanvas();
        event->accept();
        return;
    }
    QPointF delta;
    const qreal step = event->modifiers().testFlag(Qt::ShiftModifier) ? 24.0 : 8.0;
    if (event->key() == Qt::Key_Left) delta.setX(-step);
    else if (event->key() == Qt::Key_Right) delta.setX(step);
    else if (event->key() == Qt::Key_Up) delta.setY(-step);
    else if (event->key() == Qt::Key_Down) delta.setY(step);
    else if ((event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace) && m_document) {
        deleteSelection();
        event->accept();
        return;
    } else { QGraphicsView::keyPressEvent(event); return; }
    nudgeSelection(delta);
    event->accept();
}

SceneCanvas::LayerItem *SceneCanvas::layerItemAt(const QPoint &viewPosition) const {
    QGraphicsItem *item = itemAt(viewPosition);
    while (item && !dynamic_cast<LayerItem *>(item)) item = item->parentItem();
    if (auto *layer = dynamic_cast<LayerItem *>(item)) return layer;

    // QGraphicsItem::shape() covers the layer body, not the external rotate
    // handle. Resolve that handle explicitly for the selected layer so it
    // remains usable after zooming out.
    const qreal viewScale = qMax<qreal>(0.05,
        std::hypot(transform().m11(), transform().m12()));
    const qreal rotateOffset = 28.0 / viewScale;
    for (LayerItem *candidate : m_items) {
        if (!candidate->isSelected()) continue;
        const QRectF rect = candidate->rect();
        const QPointF rotateScene = candidate->mapToScene(
            QPointF(rect.center().x(), rect.top() - rotateOffset));
        if (QLineF(QPointF(viewPosition), mapFromScene(rotateScene)).length() <= 18.0)
            return candidate;
    }
    return nullptr;
}

void SceneCanvas::mousePressEvent(QMouseEvent *event) {
    if (m_presentationMode || !m_editingEnabled) {
        event->accept();
        return;
    }
    LayerItem *item = layerItemAt(event->pos());
    if (!item || event->button() != Qt::LeftButton) {
        QGraphicsView::mousePressEvent(event);
        return;
    }
    const bool add = event->modifiers().testFlag(Qt::ControlModifier);
    if (!item->isSelected()) selectLayer(item->layerId(), add);
    m_activeLayer = item->layerId();
    const SceneLayer *layer = m_document ? m_document->layer(m_format, m_activeLayer) : nullptr;
    if (!layer || layer->locked) { event->accept(); return; }
    m_beforeInteraction = selectedTransforms();
    m_pressScenePosition = mapToScene(event->pos());
    const QPointF local = item->mapFromScene(m_pressScenePosition);
    const QRectF rect = item->rect();
    const qreal viewScale = qMax<qreal>(0.05,
        std::hypot(transform().m11(), transform().m12()));
    const qreal threshold = 18.0 / viewScale;
    m_resizeEdges = 0;
    if (qAbs(local.x() - rect.left()) <= threshold) m_resizeEdges |= EdgeLeft;
    if (qAbs(local.x() - rect.right()) <= threshold) m_resizeEdges |= EdgeRight;
    if (qAbs(local.y() - rect.top()) <= threshold) m_resizeEdges |= EdgeTop;
    if (qAbs(local.y() - rect.bottom()) <= threshold) m_resizeEdges |= EdgeBottom;
    const QPointF rotatePoint(rect.center().x(), rect.top() - 28.0 / viewScale);
    // At reduced zoom the screen-sized hit radius grows in scene coordinates.
    // Give the top-center resize handle priority while the pointer is still
    // on the edge; otherwise it is easy to accidentally grab the rotate
    // affordance when trying to expand a layer upward.
    const bool topResizeHandle = (m_resizeEdges & EdgeTop) &&
        qAbs(local.x() - rect.center().x()) <= threshold;
    const bool rotateHandle = !topResizeHandle &&
        QLineF(local, rotatePoint).length() <= threshold &&
        local.y() < rect.top() - threshold * 0.75;
    if (rotateHandle) m_interaction = Interaction::Rotate;
    else if (m_resizeEdges && event->modifiers().testFlag(Qt::AltModifier)) m_interaction = Interaction::Crop;
    else if (m_resizeEdges) m_interaction = Interaction::Resize;
    else m_interaction = Interaction::Move;
    setCursor(m_interaction == Interaction::Move ? Qt::ClosedHandCursor : Qt::SizeAllCursor);
    event->accept();
}

QRectF SceneCanvas::snappedFrame(const QRectF &frame, bool disableSnap) const {
    if (!m_snapEnabled || disableSnap) return frame;
    QRectF result = frame;
    const QSize size = canvasSize();
    constexpr qreal threshold = 12.0;
    const QList<QPair<qreal, qreal>> horizontal{{frame.left(), 0.0},
                                                {frame.center().x(), size.width() / 2.0},
                                                {frame.right(), static_cast<qreal>(size.width())}};
    for (const auto &candidate : horizontal) {
        if (qAbs(candidate.first - candidate.second) <= threshold) {
            result.translate(candidate.second - candidate.first, 0); break;
        }
    }
    const QList<QPair<qreal, qreal>> vertical{{frame.top(), 0.0},
                                              {frame.center().y(), size.height() / 2.0},
                                              {frame.bottom(), static_cast<qreal>(size.height())}};
    for (const auto &candidate : vertical) {
        if (qAbs(candidate.first - candidate.second) <= threshold) {
            result.translate(0, candidate.second - candidate.first); break;
        }
    }
    return result;
}

void SceneCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (m_presentationMode || !m_editingEnabled) {
        event->accept();
        return;
    }
    if (m_interaction == Interaction::None || m_beforeInteraction.isEmpty()) {
        QGraphicsView::mouseMoveEvent(event);
        return;
    }
    const QPointF current = mapToScene(event->pos());
    const QPointF delta = current - m_pressScenePosition;
    auto next = m_beforeInteraction;
    if (m_interaction == Interaction::Move) {
        for (auto it = next.begin(); it != next.end(); ++it) {
            it->frame.translate(delta);
            it->frame = snappedFrame(it->frame, event->modifiers().testFlag(Qt::ControlModifier));
        }
    } else {
        auto it = next.find(m_activeLayer);
        if (it == next.end()) return;
        SceneTransform &value = it.value();
        if (m_interaction == Interaction::Resize) {
            QRectF frame = value.frame;
            if (m_resizeEdges & EdgeLeft) frame.setLeft(qMin(frame.right() - 32, frame.left() + delta.x()));
            if (m_resizeEdges & EdgeRight) frame.setRight(qMax(frame.left() + 32, frame.right() + delta.x()));
            if (m_resizeEdges & EdgeTop) frame.setTop(qMin(frame.bottom() - 32, frame.top() + delta.y()));
            if (m_resizeEdges & EdgeBottom) frame.setBottom(qMax(frame.top() + 32, frame.bottom() + delta.y()));
            value.frame = frame;
        } else if (m_interaction == Interaction::Crop) {
            QMarginsF crop = value.crop;
            const qreal dx = delta.x() / qMax<qreal>(1, value.frame.width());
            const qreal dy = delta.y() / qMax<qreal>(1, value.frame.height());
            if (m_resizeEdges & EdgeLeft)
                crop.setLeft(qBound<qreal>(0, crop.left() + dx, .98 - crop.right()));
            if (m_resizeEdges & EdgeRight)
                crop.setRight(qBound<qreal>(0, crop.right() - dx, .98 - crop.left()));
            if (m_resizeEdges & EdgeTop)
                crop.setTop(qBound<qreal>(0, crop.top() + dy, .98 - crop.bottom()));
            if (m_resizeEdges & EdgeBottom)
                crop.setBottom(qBound<qreal>(0, crop.bottom() - dy, .98 - crop.top()));
            value.crop = normalizedSceneCrop(crop);
        } else if (m_interaction == Interaction::Rotate) {
            const QPointF center = value.frame.center();
            value.rotationDegrees = qRadiansToDegrees(qAtan2(current.y() - center.y(),
                                                             current.x() - center.x())) + 90.0;
        }
    }
    applyTransforms(next);
    event->accept();
}

void SceneCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (m_presentationMode || !m_editingEnabled) {
        event->accept();
        return;
    }
    if (m_interaction == Interaction::None) {
        QGraphicsView::mouseReleaseEvent(event);
        return;
    }
    const auto after = selectedTransforms();
    const auto before = m_beforeInteraction;
    applyTransforms(before);
    const QString label = m_interaction == Interaction::Move ? QStringLiteral("Move layers") :
                          m_interaction == Interaction::Resize ? QStringLiteral("Resize layer") :
                          m_interaction == Interaction::Crop ? QStringLiteral("Crop layer") :
                          QStringLiteral("Rotate layer");
    m_interaction = Interaction::None;
    m_activeLayer.clear();
    m_beforeInteraction.clear();
    unsetCursor();
    pushTransforms(label, before, after);
    event->accept();
}

void SceneCanvas::drawForeground(QPainter *painter, const QRectF &rect) {
    Q_UNUSED(rect)
    if (m_presentationMode) return;
    if (!m_scene || m_scene->sceneRect().isEmpty()) return;
    painter->save();
    QPen border(QColor(QStringLiteral("#79A7FF")), 1.0);
    border.setCosmetic(true);
    painter->setPen(border);
    painter->setBrush(Qt::NoBrush);
    const QRectF bounds = m_scene->sceneRect();
    painter->drawRect(bounds);

    // Corner ticks make the true composition boundary visible even when a
    // full-size layer covers the canvas with the same dark tone as the view.
    const qreal tick = qMin<qreal>(36.0, qMin(bounds.width(), bounds.height()) * 0.04);
    const QList<QLineF> ticks {
        {bounds.topLeft(), bounds.topLeft() + QPointF(tick, 0)},
        {bounds.topLeft(), bounds.topLeft() + QPointF(0, tick)},
        {bounds.topRight(), bounds.topRight() + QPointF(-tick, 0)},
        {bounds.topRight(), bounds.topRight() + QPointF(0, tick)},
        {bounds.bottomLeft(), bounds.bottomLeft() + QPointF(tick, 0)},
        {bounds.bottomLeft(), bounds.bottomLeft() + QPointF(0, -tick)},
        {bounds.bottomRight(), bounds.bottomRight() + QPointF(-tick, 0)},
        {bounds.bottomRight(), bounds.bottomRight() + QPointF(0, -tick)}
    };
    painter->drawLines(ticks);
    painter->restore();
}

void SceneCanvas::emitSelection() {
    emit layerSelectionChanged(selectedLayerIds());
    viewport()->update();
}
