#pragma once

#include "scenedocument.h"

#include <QGraphicsView>
#include <QHash>
#include <QImage>
#include <QStringList>
#include <QUndoStack>

class QPainter;
class QContextMenuEvent;
class QWheelEvent;

class SceneCanvas final : public QGraphicsView {
    Q_OBJECT

public:
    explicit SceneCanvas(QWidget *parent = nullptr);
    ~SceneCanvas() override;

    void setDocument(SceneDocument *document, SceneFormat format);
    SceneDocument *document() const;
    SceneFormat format() const;
    void setPresentationMode(bool enabled);
    bool presentationMode() const;
    void setEditingEnabled(bool enabled);
    bool editingEnabled() const;
    void setFormat(SceneFormat format);
    QSize canvasSize() const;

    // View controls are deliberately separate from layer transforms.  The
    // canvas can be zoomed out below the fit-to-window level without changing
    // the scene document, which makes the full composition boundary easy to
    // inspect on small workspaces.
    void fitCanvas();
    void zoomIn();
    void zoomOut();
    int zoomPercent() const;

    bool selectLayer(const QString &layerId, bool add = false);
    QStringList selectedLayerIds() const;
    void clearLayerSelection();

    void nudgeSelection(const QPointF &delta);
    void fitSelection();
    void centerSelection();
    void resetSelection();
    void rotateSelection(qreal deltaDegrees);
    void cropSelection(const QMarginsF &crop);
    void setSelectionOpacity(qreal opacity);
    void setSelectionMask(SceneMask mask);
    void setSelectionGeometry(const QRectF &frame, qreal rotationDegrees);
    void setSnapEnabled(bool enabled);
    bool snapEnabled() const;
    QUndoStack *undoStack();
    void refreshFromDocument();
    void setSourcePreview(const QString &sourceId, const QImage &frame);
    bool deleteSelection();

signals:
    void sceneChanged();
    void layersChanged();
    void layerSelectionChanged(const QStringList &layerIds);
    void zoomChanged(int percent);
    void contextMenuRequested(const QString &layerId, const QPoint &globalPosition);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void drawForeground(QPainter *painter, const QRectF &rect) override;

private:
    enum class Interaction { None, Move, Resize, Crop, Rotate };
    class LayerItem;

    void rebuild();
    void syncItemsFromDocument();
    void applyTransforms(const QHash<QString, SceneTransform> &transforms);
    void pushTransforms(const QString &label,
                        const QHash<QString, SceneTransform> &before,
                        const QHash<QString, SceneTransform> &after);
    QHash<QString, SceneTransform> selectedTransforms() const;
    LayerItem *layerItemAt(const QPoint &viewPosition) const;
    QRectF snappedFrame(const QRectF &frame, bool disableSnap) const;
    void emitSelection();
    void zoomBy(qreal factor, const QPoint *viewAnchor = nullptr);
    void emitZoomChanged();

    QGraphicsScene *m_scene = nullptr;
    SceneDocument *m_document = nullptr;
    SceneFormat m_format = SceneFormat::Wide;
    QHash<QString, LayerItem *> m_items;
    QHash<QString, QImage> m_sourcePreviews;
    QUndoStack m_undoStack;
    qreal m_viewScale = 1.0;
    bool m_autoFit = true;
    bool m_presentationMode = false;
    bool m_editingEnabled = true;
    bool m_snapEnabled = true;
    Interaction m_interaction = Interaction::None;
    QString m_activeLayer;
    QPointF m_pressScenePosition;
    QHash<QString, SceneTransform> m_beforeInteraction;
    int m_resizeEdges = 0;
};
