#pragma once

#include "scenedocument.h"

#include <QGraphicsView>
#include <QHash>
#include <QImage>
#include <QStringList>
#include <QUndoStack>

class SceneCanvas final : public QGraphicsView {
    Q_OBJECT

public:
    explicit SceneCanvas(QWidget *parent = nullptr);
    ~SceneCanvas() override;

    void setDocument(SceneDocument *document, SceneFormat format);
    SceneDocument *document() const;
    SceneFormat format() const;
    void setFormat(SceneFormat format);
    QSize canvasSize() const;

    bool selectLayer(const QString &layerId, bool add = false);
    QStringList selectedLayerIds() const;
    void clearLayerSelection();

    void nudgeSelection(const QPointF &delta);
    void fitSelection();
    void centerSelection();
    void resetSelection();
    void cropSelection(const QMarginsF &crop);
    void setSelectionOpacity(qreal opacity);
    void setSelectionMask(SceneMask mask);
    void setSelectionGeometry(const QRectF &frame, qreal rotationDegrees);
    void setSnapEnabled(bool enabled);
    bool snapEnabled() const;
    QUndoStack *undoStack();
    void refreshFromDocument();
    void setSourcePreview(const QString &sourceId, const QImage &frame);

signals:
    void sceneChanged();
    void layerSelectionChanged(const QStringList &layerIds);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

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

    QGraphicsScene *m_scene = nullptr;
    SceneDocument *m_document = nullptr;
    SceneFormat m_format = SceneFormat::Wide;
    QHash<QString, LayerItem *> m_items;
    QHash<QString, QImage> m_sourcePreviews;
    QUndoStack m_undoStack;
    bool m_snapEnabled = true;
    Interaction m_interaction = Interaction::None;
    QString m_activeLayer;
    QPointF m_pressScenePosition;
    QHash<QString, SceneTransform> m_beforeInteraction;
    int m_resizeEdges = 0;
};
