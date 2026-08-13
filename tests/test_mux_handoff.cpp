#include "renderers/mux_renderer.h"
#include "uxplay_api.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QVector>
#include <QtTest>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <utility>

namespace {
void discardLog(void *, int, const char *) {}
}

class MuxHandoffTest final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        gst_init(nullptr, nullptr);

        GError *error = nullptr;
        GstElement *source = gst_parse_launch(
            "videotestsrc num-buffers=18 ! video/x-raw,width=320,height=180,framerate=30/1 "
            "! videoconvert ! openh264enc gop-size=6 ! h264parse config-interval=-1 "
            "! video/x-h264,stream-format=byte-stream,alignment=au "
            "! appsink name=encoded sync=false", &error);
        QVERIFY2(source && !error, error ? error->message : "Could not create encoded test stream");
        if (error) g_error_free(error);

        GstElement *sink = gst_bin_get_by_name(GST_BIN(source), "encoded");
        QVERIFY(sink);
        gst_element_set_state(source, GST_STATE_PLAYING);
        while (GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 2 * GST_SECOND)) {
            GstBuffer *buffer = gst_sample_get_buffer(sample);
            GstMapInfo map;
            if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                m_frames.append(QByteArray(reinterpret_cast<const char *>(map.data),
                                           static_cast<qsizetype>(map.size)));
                gst_buffer_unmap(buffer, &map);
            }
            gst_sample_unref(sample);
        }
        gst_element_set_state(source, GST_STATE_NULL);
        gst_object_unref(sink);
        gst_object_unref(source);
        QVERIFY(m_frames.size() >= 12);
    }

    void init() {
        m_logger = logger_init();
        QVERIFY(m_logger);
        logger_set_callback(m_logger, discardLog, nullptr);
        mux_renderer_set_test_handoff_limits(0, 0);
        mux_renderer_set_test_consumer_delay(0);
        mux_renderer_reset_video_cache();
        for (const QByteArray &frame : std::as_const(m_frames)) {
            mux_renderer_cache_video(reinterpret_cast<unsigned char *>(const_cast<char *>(frame.constData())),
                                     static_cast<int>(frame.size()), false);
        }
    }

    void cleanup() {
        uxplay_set_recording_test_stop_result(1);
        uxplay_stop_recording();
        uxplay_set_recording_test_mode(0);
        mux_renderer_destroy();
        mux_renderer_set_test_handoff_limits(0, 0);
        mux_renderer_set_test_consumer_delay(0);
        logger_destroy(m_logger);
        m_logger = nullptr;
    }

    void slowConsumerDoesNotDelayIngressAndStopDrains() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("video")).toUtf8();
        mux_renderer_set_test_handoff_limits(32, 8 * 1024 * 1024);
        mux_renderer_set_test_consumer_delay(150);
        mux_renderer_init(m_logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));

        QElapsedTimer ingress;
        ingress.start();
        for (int index = 0; index < 4; ++index) {
            QByteArray &frame = m_frames[index];
            QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(frame.data()),
                                            static_cast<int>(frame.size()),
                                            static_cast<uint64_t>(index) * GST_SECOND / 30));
        }
        QVERIFY2(ingress.elapsed() < 200,
                 qPrintable(QStringLiteral("Ingress waited %1 ms for a deliberately slow consumer")
                                .arg(ingress.elapsed())));

        QElapsedTimer drain;
        drain.start();
        QVERIFY(mux_renderer_stop());
        QVERIFY2(drain.elapsed() >= 300,
                 qPrintable(QStringLiteral("Stop returned before queued media drained (%1 ms)")
                                .arg(drain.elapsed())));
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00000.mkv"))).size() > 0);
    }

    void boundedQueueReportsOverflowAndKeepsFinalizedPrefix() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("video")).toUtf8();
        mux_renderer_set_test_handoff_limits(1, 8 * 1024 * 1024);
        mux_renderer_set_test_consumer_delay(250);
        mux_renderer_init(m_logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));

        QByteArray &first = m_frames[0];
        QByteArray &second = m_frames[1];
        QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(first.data()),
                                        static_cast<int>(first.size()), 0));
        QElapsedTimer rejected;
        rejected.start();
        QVERIFY(!mux_renderer_push_video(reinterpret_cast<unsigned char *>(second.data()),
                                         static_cast<int>(second.size()), GST_SECOND / 30));
        QVERIFY(rejected.elapsed() < 100);
        QVERIFY(mux_renderer_has_failed());
        QVERIFY(!mux_renderer_stop());
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00000.mkv"))).size() > 0);
    }

    void reconnectBoundaryFinalizesBeforeFollowingCodecAndFrames() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("video")).toUtf8();
        mux_renderer_init(m_logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));

        for (int index = 0; index < 4; ++index) {
            QByteArray &frame = m_frames[index];
            QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(frame.data()),
                                            static_cast<int>(frame.size()),
                                            static_cast<uint64_t>(index) * GST_SECOND / 30));
        }
        QVERIFY(mux_renderer_queue_stop());
        QVERIFY(mux_renderer_queue_video_codec(false));
        for (int index = 0; index < 4; ++index) {
            QByteArray &frame = m_frames[index];
            QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(frame.data()),
                                            static_cast<int>(frame.size()),
                                            static_cast<uint64_t>(index) * GST_SECOND / 30));
        }
        QVERIFY(mux_renderer_stop());
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00000.mkv"))).size() > 0);
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00001.mkv"))).size() > 0);
    }

    void lateFrameAfterReconnectStopIsDroppedUntilCodecBoundary() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("video")).toUtf8();
        mux_renderer_set_test_consumer_delay(100);
        mux_renderer_init(m_logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));

        QByteArray &first = m_frames[0];
        QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(first.data()),
                                        static_cast<int>(first.size()), 0));
        QVERIFY(mux_renderer_queue_stop());
        QByteArray &late = m_frames[1];
        bool accepted = true;
        QVERIFY(mux_renderer_push_video_with_acceptance(
            reinterpret_cast<unsigned char *>(late.data()),
            static_cast<int>(late.size()), GST_SECOND / 30, &accepted));
        QVERIFY(!accepted);
        QVERIFY(!mux_renderer_has_failed());

        QVERIFY(mux_renderer_queue_video_codec(false));
        for (int index = 0; index < 4; ++index) {
            QByteArray &frame = m_frames[index];
            QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(frame.data()),
                                            static_cast<int>(frame.size()),
                                            static_cast<uint64_t>(index) * GST_SECOND / 30));
        }
        QVERIFY(mux_renderer_stop());
        QVERIFY(!mux_renderer_has_failed());
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00000.mkv"))).size() > 0);
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00001.mkv"))).size() > 0);
    }

    void disabledVideoIsReportedNotAccepted() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("audio-only")).toUtf8();
        mux_renderer_init(m_logger, prefix.constData(), true, false);

        QByteArray &frame = m_frames[0];
        bool accepted = true;
        QVERIFY(mux_renderer_push_video_with_acceptance(
            reinterpret_cast<unsigned char *>(frame.data()),
            static_cast<int>(frame.size()), 0, &accepted));
        QVERIFY(!accepted);
    }

    void emptyReconnectSegmentDoesNotPoisonFollowingSegment() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QByteArray prefix = QDir(temp.path()).filePath(QStringLiteral("video")).toUtf8();
        mux_renderer_init(m_logger, prefix.constData(), false, true);
        QVERIFY(mux_renderer_choose_video_codec(false));
        QVERIFY(mux_renderer_queue_stop());
        QVERIFY(mux_renderer_queue_video_codec(false));

        for (int index = 0; index < 4; ++index) {
            QByteArray &frame = m_frames[index];
            QVERIFY(mux_renderer_push_video(reinterpret_cast<unsigned char *>(frame.data()),
                                            static_cast<int>(frame.size()),
                                            static_cast<uint64_t>(index) * GST_SECOND / 30));
        }
        QVERIFY(mux_renderer_stop());
        QVERIFY(!mux_renderer_has_failed());
        QVERIFY(QFileInfo(QDir(temp.path()).filePath(QStringLiteral("video-00000.mkv"))).size() > 0);
    }

    void publicStatusPropagatesStartAndStopFailure() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        uxplay_set_recording_test_mode(1);

        QVERIFY(uxplay_start_recording(temp.path().toUtf8().constData()));
        QCOMPARE(uxplay_get_recording_status(), UXPLAY_RECORDING_ACTIVE);
        uxplay_set_recording_test_stop_result(0);
        QVERIFY(!uxplay_stop_recording());
        QCOMPARE(uxplay_get_recording_status(), UXPLAY_RECORDING_FAILED);

        uxplay_set_recording_test_stop_result(1);
        QVERIFY(uxplay_start_recording(temp.path().toUtf8().constData()));
        QCOMPARE(uxplay_get_recording_status(), UXPLAY_RECORDING_ACTIVE);
        QVERIFY(uxplay_stop_recording());
        QCOMPARE(uxplay_get_recording_status(), UXPLAY_RECORDING_INACTIVE);
    }

private:
    QVector<QByteArray> m_frames;
    logger_t *m_logger = nullptr;
};

QTEST_APPLESS_MAIN(MuxHandoffTest)
#include "test_mux_handoff.moc"
