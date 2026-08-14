#include "studio/scenecanvas.h"

#include <QImage>
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

    void deleteSelectionSkipsLockedLayers() {
        SceneDocument document;
        const QString first = document.addSource(SceneSourceType::Text, QStringLiteral("First"));
        const QString second = document.addSource(SceneSourceType::Text, QStringLiteral("Second"));
        const QString firstLayer = document.addLayer(SceneFormat::Wide, first);
        const QString secondLayer = document.addLayer(SceneFormat::Wide, second);
        QVERIFY(document.setLayerLocked(SceneFormat::Wide, firstLayer, true));

        SceneCanvas canvas;
        canvas.setDocument(&document, SceneFormat::Wide);
        QVERIFY(canvas.selectLayer(firstLayer));
        QVERIFY(!canvas.deleteSelection());
        QCOMPARE(document.composition(SceneFormat::Wide).layers.size(), 2);

        QVERIFY(canvas.selectLayer(secondLayer));
        QVERIFY(canvas.deleteSelection());
        QCOMPARE(document.composition(SceneFormat::Wide).layers.size(), 1);
        QCOMPARE(document.composition(SceneFormat::Wide).layers.first().id, firstLayer);
    }

    void zoomControlsDoNotChangeSceneCoordinates() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::Color,
                                                   QStringLiteral("Background"));
        document.addLayer(SceneFormat::Wide, source);

        SceneCanvas canvas;
        canvas.resize(960, 600);
        canvas.setDocument(&document, SceneFormat::Wide);
        const QRectF sceneBounds = canvas.sceneRect();
        QCOMPARE(canvas.zoomPercent(), 100);

        canvas.zoomOut();
        QVERIFY(canvas.zoomPercent() < 100);
        QCOMPARE(canvas.sceneRect(), sceneBounds);
        canvas.zoomIn();
        QCOMPARE(canvas.zoomPercent(), 100);
        canvas.zoomOut();
        canvas.fitCanvas();
        QCOMPARE(canvas.zoomPercent(), 100);
        QCOMPARE(canvas.sceneRect(), sceneBounds);
    }

    void clearingSourcePreviewRemovesTheLastFrame() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::AirPlay,
                                                   QStringLiteral("iPad"));
        const QString layer = document.addLayer(SceneFormat::Wide, source);

        SceneCanvas canvas;
        canvas.resize(960, 600);
        canvas.setDocument(&document, SceneFormat::Wide);
        canvas.show();
        QTRY_VERIFY(canvas.isVisible());
        QImage frame(32, 18, QImage::Format_ARGB32_Premultiplied);
        frame.fill(QColor(QStringLiteral("#e45767")));
        canvas.setSourcePreview(source, frame);
        QCoreApplication::processEvents();
        const QPoint sample = canvas.mapFromScene(QPointF(480, 480));
        const QColor renderedFrame = canvas.viewport()->grab().toImage().pixelColor(sample);
        QCOMPARE(renderedFrame.red(), 228);
        QCOMPARE(renderedFrame.green(), 87);
        QCOMPARE(renderedFrame.blue(), 103);
        canvas.setSourcePreview(source, {});
        canvas.setFormat(SceneFormat::Vertical);
        canvas.setFormat(SceneFormat::Wide);
        QCoreApplication::processEvents();
        const QColor placeholder = canvas.viewport()->grab().toImage().pixelColor(sample);
        QVERIFY(placeholder != renderedFrame);
        QVERIFY(document.layer(SceneFormat::Wide, layer) != nullptr);
    }

    void topResizeHandleWinsOverRotateHandleAtZoomOut() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::Color,
                                                   QStringLiteral("Resizable"));
        const QString layerId = document.addLayer(SceneFormat::Wide, source);
        SceneTransform transform;
        transform.frame = QRectF(560, 320, 480, 300);
        QVERIFY(document.setTransform(SceneFormat::Wide, layerId, transform));

        SceneCanvas canvas;
        canvas.resize(960, 600);
        canvas.setDocument(&document, SceneFormat::Wide);
        canvas.show();
        QTRY_VERIFY(canvas.isVisible());
        QVERIFY(canvas.selectLayer(layerId));
        canvas.zoomOut();
        canvas.zoomOut();

        const QRectF before = document.layer(SceneFormat::Wide, layerId)->transform.frame;
        const QPoint topHandle = canvas.mapFromScene(QPointF(before.center().x(), before.top()));
        QTest::mousePress(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, topHandle);
        QTest::mouseMove(canvas.viewport(), topHandle - QPoint(0, 24));
        QTest::mouseRelease(canvas.viewport(), Qt::LeftButton, Qt::NoModifier, topHandle - QPoint(0, 24));

        const SceneTransform after = document.layer(SceneFormat::Wide, layerId)->transform;
        QVERIFY(after.frame.top() < before.top());
        QVERIFY(qFuzzyIsNull(after.rotationDegrees));
    }
};

QTEST_MAIN(SceneCanvasTest)
#include "test_scenecanvas.moc"
