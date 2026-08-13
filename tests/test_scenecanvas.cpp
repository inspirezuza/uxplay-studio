#include "studio/scenecanvas.h"

#include <QSignalSpy>
#include <QtTest>

class SceneCanvasTest final : public QObject {
    Q_OBJECT

private slots:
    void editsSelectionWithUndoAndRedo() {
        SceneDocument document;
        const QString camera = document.addSource(SceneSourceType::Camera,
                                                  QStringLiteral("Presenter"));
        const QString layer = document.addLayer(SceneFormat::Wide, camera);
        SceneTransform initial;
        initial.frame = QRectF(100, 120, 480, 320);
        QVERIFY(document.setTransform(SceneFormat::Wide, layer, initial));

        SceneCanvas canvas;
        canvas.resize(960, 600);
        canvas.setDocument(&document, SceneFormat::Wide);
        QVERIFY(canvas.selectLayer(layer));
        QSignalSpy changed(&canvas, &SceneCanvas::sceneChanged);

        canvas.nudgeSelection(QPointF(8, -4));
        QCOMPARE(document.layer(SceneFormat::Wide, layer)->transform.frame,
                 QRectF(108, 116, 480, 320));
        QCOMPARE(changed.count(), 1);
        QVERIFY(canvas.undoStack()->canUndo());

        canvas.undoStack()->undo();
        QCOMPARE(document.layer(SceneFormat::Wide, layer)->transform.frame,
                 QRectF(100, 120, 480, 320));
        canvas.undoStack()->redo();
        QCOMPARE(document.layer(SceneFormat::Wide, layer)->transform.frame,
                 QRectF(108, 116, 480, 320));
    }

    void preservesIndependentSelectionsWhenSwitchingFormats() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::AirPlay,
                                                  QStringLiteral("iPad"));
        const QString wide = document.addLayer(SceneFormat::Wide, source);
        const QString vertical = document.addLayer(SceneFormat::Vertical, source);

        SceneCanvas canvas;
        canvas.setDocument(&document, SceneFormat::Wide);
        QVERIFY(canvas.selectLayer(wide));
        QCOMPARE(canvas.selectedLayerIds(), QStringList({wide}));
        canvas.setFormat(SceneFormat::Vertical);
        QVERIFY(canvas.selectLayer(vertical));
        QCOMPARE(canvas.selectedLayerIds(), QStringList({vertical}));
        QCOMPARE(canvas.canvasSize(), QSize(1080, 1920));
    }

    void fitAndCropCommandsUseCanvasCoordinates() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::Image,
                                                  QStringLiteral("Reference"));
        const QString layer = document.addLayer(SceneFormat::Vertical, source);
        SceneCanvas canvas;
        canvas.setDocument(&document, SceneFormat::Vertical);
        QVERIFY(canvas.selectLayer(layer));

        canvas.fitSelection();
        QCOMPARE(document.layer(SceneFormat::Vertical, layer)->transform.frame,
                 QRectF(0, 0, 1080, 1920));
        canvas.cropSelection(QMarginsF(0.1, 0.2, 0.1, 0.0));
        QCOMPARE(document.layer(SceneFormat::Vertical, layer)->transform.crop,
                 QMarginsF(0.1, 0.2, 0.1, 0.0));

        canvas.cropSelection(QMarginsF(0.8, 0.9, 0.8, 0.9));
        const QMarginsF normalized = document.layer(SceneFormat::Vertical, layer)->transform.crop;
        QVERIFY(normalized.left() + normalized.right() <= 0.98);
        QVERIFY(normalized.top() + normalized.bottom() <= 0.98);
    }

    void exactGeometryEditsAreUndoable() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::Text,
                                                  QStringLiteral("Title"));
        const QString layer = document.addLayer(SceneFormat::Wide, source);
        SceneCanvas canvas;
        canvas.setDocument(&document, SceneFormat::Wide);
        QVERIFY(canvas.selectLayer(layer));

        canvas.setSelectionGeometry(QRectF(240, 180, 960, 200), 12.5);
        const SceneTransform edited = document.layer(SceneFormat::Wide, layer)->transform;
        QCOMPARE(edited.frame, QRectF(240, 180, 960, 200));
        QCOMPARE(edited.rotationDegrees, 12.5);
        QVERIFY(canvas.undoStack()->canUndo());

        canvas.undoStack()->undo();
        QVERIFY(document.layer(SceneFormat::Wide, layer)->transform != edited);
        canvas.undoStack()->redo();
        QCOMPARE(document.layer(SceneFormat::Wide, layer)->transform, edited);
    }
};

QTEST_MAIN(SceneCanvasTest)
#include "test_scenecanvas.moc"
