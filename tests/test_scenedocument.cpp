#include "studio/scenedocument.h"

#include <QJsonDocument>
#include <QtTest>

class SceneDocumentTest final : public QObject {
    Q_OBJECT

private slots:
    void keepsWideAndVerticalLayoutsIndependent() {
        SceneDocument document;
        const QString sourceId = document.addSource(SceneSourceType::Camera,
                                                    QStringLiteral("Presenter"));
        const QString wideLayer = document.addLayer(SceneFormat::Wide, sourceId);
        const QString verticalLayer = document.addLayer(SceneFormat::Vertical, sourceId);

        QVERIFY(!wideLayer.isEmpty());
        QVERIFY(!verticalLayer.isEmpty());
        QVERIFY(wideLayer != verticalLayer);

        SceneTransform wide;
        wide.frame = QRectF(1200, 640, 480, 360);
        wide.crop = QMarginsF(0.1, 0.0, 0.2, 0.0);
        wide.rotationDegrees = -3.0;
        QVERIFY(document.setTransform(SceneFormat::Wide, wideLayer, wide));

        SceneTransform vertical;
        vertical.frame = QRectF(90, 1120, 900, 700);
        QVERIFY(document.setTransform(SceneFormat::Vertical, verticalLayer, vertical));

        QCOMPARE(document.layer(SceneFormat::Wide, wideLayer)->transform.frame,
                 QRectF(1200, 640, 480, 360));
        QCOMPARE(document.layer(SceneFormat::Vertical, verticalLayer)->transform.frame,
                 QRectF(90, 1120, 900, 700));
        QCOMPARE(document.composition(SceneFormat::Wide).canvasSize, QSize(1920, 1080));
        QCOMPARE(document.composition(SceneFormat::Vertical).canvasSize, QSize(1080, 1920));
    }

    void reordersVisibilityAndLockThroughTheDocumentInterface() {
        SceneDocument document;
        const QString airplay = document.addSource(SceneSourceType::AirPlay,
                                                   QStringLiteral("iPad"));
        const QString camera = document.addSource(SceneSourceType::Camera,
                                                  QStringLiteral("Camera"));
        const QString airplayLayer = document.addLayer(SceneFormat::Wide, airplay);
        const QString cameraLayer = document.addLayer(SceneFormat::Wide, camera);

        QCOMPARE(document.composition(SceneFormat::Wide).layers.last().id, cameraLayer);
        QVERIFY(document.moveLayer(SceneFormat::Wide, airplayLayer, 1));
        QCOMPARE(document.composition(SceneFormat::Wide).layers.last().id, airplayLayer);
        QVERIFY(document.setLayerVisible(SceneFormat::Wide, cameraLayer, false));
        QVERIFY(document.setLayerLocked(SceneFormat::Wide, cameraLayer, true));
        QVERIFY(!document.layer(SceneFormat::Wide, cameraLayer)->visible);
        QVERIFY(document.layer(SceneFormat::Wide, cameraLayer)->locked);
    }

    void roundTripsVersionedProjectData() {
        SceneDocument original;
        original.setTitle(QStringLiteral("Goodnotes lesson"));
        const QString camera = original.addSource(SceneSourceType::Camera,
                                                  QStringLiteral("USB Camera"));
        const QString layerId = original.addLayer(SceneFormat::Vertical, camera);
        SceneTransform transform;
        transform.frame = QRectF(80, 1220, 920, 620);
        transform.crop = QMarginsF(0.05, 0.1, 0.15, 0.0);
        transform.rotationDegrees = 2.5;
        transform.opacity = 0.82;
        transform.mask = SceneMask::RoundedRectangle;
        QVERIFY(original.setTransform(SceneFormat::Vertical, layerId, transform));

        const QJsonObject json = original.toJson();
        QCOMPARE(json.value(QStringLiteral("schemaVersion")).toInt(), 1);

        const auto loaded = SceneDocument::fromJson(json);
        QVERIFY2(loaded.has_value(), qPrintable(loaded.error));
        QCOMPARE(loaded.document->title(), QStringLiteral("Goodnotes lesson"));
        const SceneLayer *layer = loaded.document->layer(SceneFormat::Vertical, layerId);
        QVERIFY(layer);
        QCOMPARE(layer->transform.frame, QRectF(80, 1220, 920, 620));
        QCOMPARE(layer->transform.crop, QMarginsF(0.05, 0.1, 0.15, 0.0));
        QCOMPARE(layer->transform.rotationDegrees, 2.5);
        QCOMPARE(layer->transform.opacity, 0.82);
        QCOMPARE(layer->transform.mask, SceneMask::RoundedRectangle);
    }

    void normalizesOpposingCropMarginsToKeepVisibleContent() {
        SceneDocument document;
        const QString source = document.addSource(SceneSourceType::AirPlay,
                                                   QStringLiteral("iPad"));
        const QString layer = document.addLayer(SceneFormat::Wide, source);
        SceneTransform transform = document.layer(SceneFormat::Wide, layer)->transform;
        transform.crop = QMarginsF(0.8, -0.2, 0.7, 2.0);

        QVERIFY(document.setTransform(SceneFormat::Wide, layer, transform));
        const QMarginsF crop = document.layer(SceneFormat::Wide, layer)->transform.crop;
        QVERIFY(crop.left() >= 0.0);
        QCOMPARE(crop.top(), 0.0);
        QVERIFY(crop.left() + crop.right() <= 0.98);
        QVERIFY(crop.top() + crop.bottom() <= 0.98);
        QVERIFY(1.0 - crop.left() - crop.right() > 0.019);
        QVERIFY(1.0 - crop.top() - crop.bottom() > 0.019);
    }
};

QTEST_GUILESS_MAIN(SceneDocumentTest)
#include "test_scenedocument.moc"
