/**
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2021-24 F. Duncanh
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <glib/gstdio.h>
#include "mux_renderer.h"

#define SECOND_IN_NSECS 1000000000UL
#define DEFAULT_HANDOFF_MAX_ITEMS 512
#define DEFAULT_HANDOFF_MAX_BYTES (64 * 1024 * 1024)

static logger_t *logger = NULL;
static char *output_filename = NULL;
static int file_count = 0;
static gboolean no_audio = FALSE;
static gboolean no_video = FALSE;
static gboolean audio_is_alac = FALSE;
static gboolean video_is_h265 = FALSE;
static gboolean direct_video_only = FALSE;
static gboolean mux_session_clean = TRUE;
static gint next_direct_fragment = 0;
static GBytes *cached_video_config = NULL;
static gboolean cached_video_is_h265 = FALSE;
static gint cached_video_codec = -1;
static GMutex video_config_mutex;

typedef enum mux_command_type_e {
    MUX_COMMAND_VIDEO,
    MUX_COMMAND_AUDIO,
    MUX_COMMAND_VIDEO_CODEC,
    MUX_COMMAND_AUDIO_CODEC,
    MUX_COMMAND_STOP,
    MUX_COMMAND_QUIT
} mux_command_type_t;

typedef struct mux_completion_s {
    GMutex mutex;
    GCond condition;
    gboolean done;
    gboolean result;
} mux_completion_t;

typedef struct mux_command_s {
    mux_command_type_t type;
    guint8 *data;
    gint data_len;
    guint64 ntp_time;
    gboolean is_h265;
    guint8 audio_ct;
    gboolean require_video_data;
    gboolean internal_stop;
    gboolean reserved;
    mux_completion_t *completion;
} mux_command_t;

static GAsyncQueue *handoff_queue = NULL;
static GThread *handoff_worker = NULL;
static volatile gint handoff_shutdown = TRUE;
static volatile gint handoff_accepting = FALSE;
static volatile gint handoff_may_accept = FALSE;
static volatile gint handoff_failed = FALSE;
static volatile gint handoff_users = 0;
static volatile gint handoff_items = 0;
static volatile gint handoff_bytes = 0;
static volatile gint handoff_max_items = DEFAULT_HANDOFF_MAX_ITEMS;
static volatile gint handoff_max_bytes = DEFAULT_HANDOFF_MAX_BYTES;
static volatile gint test_consumer_delay_ms = 0;

typedef struct mux_renderer_s {
    GstElement *pipeline;
    GstElement *video_appsrc;
    GstElement *audio_appsrc;
    GstElement *filesink;
    GstBus *bus;
    GstClockTime base_time;
    GstClockTime first_video_time;
    GstClockTime first_audio_time;
    gboolean audio_started;
    gboolean is_alac;
    gboolean is_h265;
    gboolean failed;
    guint64 video_buffers;
    gint first_fragment;
} mux_renderer_t;

static mux_renderer_t *renderer = NULL;

static bool mux_renderer_start_internal(void);
static bool mux_renderer_stop_internal(bool require_video_data);
static void mux_renderer_destroy_internal(void);
static gpointer handoff_worker_main(gpointer unused);

static void mark_handoff_failed(const char *message) {
    if (g_atomic_int_compare_and_exchange(&handoff_failed, FALSE, TRUE)) {
        if (logger && message) logger_log(logger, LOGGER_ERR, "%s", message);
    }
}

static void completion_init(mux_completion_t *completion) {
    g_mutex_init(&completion->mutex);
    g_cond_init(&completion->condition);
    completion->done = FALSE;
    completion->result = FALSE;
}

static void completion_finish(mux_completion_t *completion, gboolean result) {
    if (!completion) return;
    g_mutex_lock(&completion->mutex);
    completion->result = result;
    completion->done = TRUE;
    g_cond_signal(&completion->condition);
    g_mutex_unlock(&completion->mutex);
}

static gboolean completion_wait(mux_completion_t *completion) {
    g_mutex_lock(&completion->mutex);
    while (!completion->done) g_cond_wait(&completion->condition, &completion->mutex);
    const gboolean result = completion->result;
    g_mutex_unlock(&completion->mutex);
    g_cond_clear(&completion->condition);
    g_mutex_clear(&completion->mutex);
    return result;
}

static void release_handoff_reservation(const mux_command_t *command) {
    if (!command || !command->reserved) return;
    if (command->data_len > 0) g_atomic_int_add(&handoff_bytes, -command->data_len);
    g_atomic_int_add(&handoff_items, -1);
}

static void free_command(mux_command_t *command) {
    if (!command) return;
    g_free(command->data);
    g_free(command);
}

static gboolean reserve_counter(volatile gint *counter, gint amount, gint limit) {
    for (;;) {
        const gint current = g_atomic_int_get(counter);
        if (amount < 0 || current > limit - amount) return FALSE;
        if (g_atomic_int_compare_and_exchange(counter, current, current + amount)) return TRUE;
    }
}

static gboolean reserve_handoff(gint data_len) {
    const gint max_items = g_atomic_int_get(&handoff_max_items);
    const gint max_bytes = g_atomic_int_get(&handoff_max_bytes);
    if (!reserve_counter(&handoff_items, 1, max_items)) return FALSE;
    if (!reserve_counter(&handoff_bytes, data_len, max_bytes)) {
        g_atomic_int_add(&handoff_items, -1);
        return FALSE;
    }
    return TRUE;
}

static void wait_for_handoff_users(void) {
    while (g_atomic_int_get(&handoff_users) != 0) g_thread_yield();
}

static void discard_partial_renderer(void) {
    if (!renderer) return;
    if (renderer->pipeline) gst_element_set_state(renderer->pipeline, GST_STATE_NULL);
    if (renderer->video_appsrc) gst_object_unref(renderer->video_appsrc);
    if (renderer->audio_appsrc) gst_object_unref(renderer->audio_appsrc);
    if (renderer->filesink) gst_object_unref(renderer->filesink);
    if (renderer->bus) gst_object_unref(renderer->bus);
    if (renderer->pipeline) gst_object_unref(renderer->pipeline);
    g_free(renderer);
    renderer = NULL;
}

static const char h264_caps[] = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
static const char h265_caps[] = "video/x-h265,stream-format=(string)byte-stream,alignment=(string)au";

void mux_renderer_reset_video_cache(void) {
    g_mutex_lock(&video_config_mutex);
    if (cached_video_config) {
        g_bytes_unref(cached_video_config);
        cached_video_config = NULL;
    }
    cached_video_is_h265 = FALSE;
    g_atomic_int_set(&cached_video_codec, -1);
    g_mutex_unlock(&video_config_mutex);
}

void mux_renderer_cache_video(unsigned char *data, int data_len, bool is_h265) {
    if (!data || data_len < 6) return;
    if (g_atomic_int_get(&cached_video_codec) == (is_h265 ? 1 : 0)) return;
    gboolean contains_parameter_set = FALSE;
    for (int i = 0; i + 5 < data_len; ++i) {
        int offset = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) offset = 3;
        else if (i + 4 < data_len && data[i] == 0 && data[i + 1] == 0 &&
                 data[i + 2] == 0 && data[i + 3] == 1) offset = 4;
        if (!offset) continue;
        const unsigned char type = is_h265 ? (data[i + offset] >> 1) & 0x3f
                                           : data[i + offset] & 0x1f;
        if ((!is_h265 && (type == 7 || type == 8)) ||
            (is_h265 && (type == 32 || type == 33 || type == 34))) {
            contains_parameter_set = TRUE;
            break;
        }
    }
    if (!contains_parameter_set) return;
    GBytes *replacement = g_bytes_new(data, (gsize) data_len);
    g_mutex_lock(&video_config_mutex);
    if (cached_video_config) g_bytes_unref(cached_video_config);
    cached_video_config = replacement;
    cached_video_is_h265 = is_h265;
    g_atomic_int_set(&cached_video_codec, is_h265 ? 1 : 0);
    g_mutex_unlock(&video_config_mutex);
}

static const char aac_eld_caps[] = "audio/mpeg,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)f8e85000";
static const char alac_caps[] = "audio/x-alac,mpegversion=(int)4,channels=(int)2,rate=(int)44100,stream-format=raw,codec_data=(buffer)"
                                "00000024""616c6163""00000000""00000160""0010280a""0e0200ff""00000000""00000000""0000ac44";

/* called once when uxplay first starts */
void mux_renderer_init(logger_t *render_logger, const char *filename, bool use_audio, bool use_video) {
    mux_renderer_destroy();
    logger = render_logger;
    no_audio = !use_audio;
    no_video = !use_video;
    direct_video_only = no_audio && !no_video;
    mux_session_clean = TRUE;
    if (direct_video_only) g_atomic_int_set(&next_direct_fragment, 0);
    if (no_audio && no_video) {
        logger_log(logger, LOGGER_INFO, "both audio and video rendering are disabled: nothing to record: (not starting mux renderer)");
        return;
    } else if (no_audio) {
        logger_log(logger, LOGGER_INFO, "audio rendering is disabled: video only will be recorded");
    } else if (no_video) {
        logger_log(logger, LOGGER_INFO, "video rendering is disabled: audio only will be recorded");
    }
    g_free(output_filename);
    output_filename = g_strdup(filename);
    file_count = 0;
    g_atomic_int_set(&handoff_items, 0);
    g_atomic_int_set(&handoff_bytes, 0);
    g_atomic_int_set(&handoff_failed, FALSE);
    g_atomic_int_set(&handoff_accepting, FALSE);
    g_atomic_int_set(&handoff_may_accept, TRUE);
    g_atomic_int_set(&handoff_shutdown, FALSE);
    handoff_queue = g_async_queue_new();
    GError *thread_error = NULL;
    handoff_worker = g_thread_try_new("uxplay-mux", handoff_worker_main, NULL, &thread_error);
    if (!handoff_worker) {
        logger_log(logger, LOGGER_ERR, "Could not create mux handoff worker: %s",
                   thread_error ? thread_error->message : "unknown error");
        g_clear_error(&thread_error);
        g_async_queue_unref(handoff_queue);
        handoff_queue = NULL;
        g_atomic_int_set(&handoff_shutdown, TRUE);
        mark_handoff_failed(NULL);
        return;
    }
    logger_log(logger, LOGGER_INFO, "Mux renderer initialized: %s", output_filename);
}

static
gchar *direct_fragment_location(GstElement *splitmux, guint fragment_id, gpointer user_data) {
    (void) splitmux;
    (void) user_data;
    g_atomic_int_set(&next_direct_fragment, (gint) fragment_id + 1);
    return g_strdup_printf("%s-%05u.mkv", output_filename, fragment_id);
}

static gboolean direct_output_exists(gint first_fragment) {
    if (!direct_video_only || !output_filename) return TRUE;
    const gint fragment_count = g_atomic_int_get(&next_direct_fragment);
    for (gint fragment = first_fragment; fragment < fragment_count; ++fragment) {
        gchar *path = g_strdup_printf("%s-%05d.mkv", output_filename, fragment);
        GStatBuf stat_buffer;
        const gboolean valid = g_stat(path, &stat_buffer) == 0 && stat_buffer.st_size > 0;
        g_free(path);
        if (valid) return TRUE;
    }
    return FALSE;
}

static
bool mux_renderer_start_internal(void) {
    GError *error = NULL;
    GstCaps *video_caps = NULL;
    GstCaps *audio_caps = NULL;

    if (renderer && renderer->pipeline) {
        logger_log(logger, LOGGER_DEBUG, "Mux renderer already running");
        return mux_session_clean && !renderer->failed;
    }

    mux_renderer_destroy_internal();
    
    renderer = g_new0(mux_renderer_t, 1);
    renderer->base_time = GST_CLOCK_TIME_NONE;
    renderer->first_video_time = GST_CLOCK_TIME_NONE;
    renderer->first_audio_time = GST_CLOCK_TIME_NONE;
    renderer->audio_started = FALSE;
    renderer->pipeline = NULL;
    renderer->video_appsrc = NULL;
    renderer->audio_appsrc = NULL;
    renderer->is_alac = audio_is_alac;
    renderer->is_h265 = video_is_h265;
    renderer->first_fragment = g_atomic_int_get(&next_direct_fragment);

    file_count++;
    GString *filename = g_string_new("");
    if (direct_video_only) {
        g_string_append_printf(filename, "%s-%%05d.mkv", output_filename);
    } else {
        g_string_append_printf(filename, "%s.%d.", output_filename, file_count);
    }
    if (!direct_video_only && !no_video && !audio_is_alac) {
        if (video_is_h265) {
            g_string_append(filename,"H265.");
        } else {
            g_string_append(filename,"H264.");
        }
    } if (!direct_video_only && !no_audio) {
        if (audio_is_alac) {
            g_string_append(filename,"ALAC.");
        } else {
            g_string_append(filename,"AAC.");
        }
    }
    if (!direct_video_only) g_string_append(filename, "mp4");
    
    GString *launch = g_string_new("");

    if (!no_video && !audio_is_alac) {
        g_string_append(launch, "appsrc name=video_src format=time is-live=true ! queue ! ");
        if (video_is_h265) {
            g_string_append(launch, "h265parse ! ");
        } else {
            g_string_append(launch, "h264parse ! ");
        }
        g_string_append(launch, "mux. ");
    }
    if (!no_audio) {
        g_string_append(launch, "appsrc name=audio_src format=time is-live=true ! queue ! ");
        if (!audio_is_alac ) {
            g_string_append(launch, "aacparse ! queue ! ");
        }
        g_string_append(launch, "mux. ");
    }
    if (direct_video_only) {
        g_string_append_printf(launch,
            "splitmuxsink name=mux muxer-factory=matroskamux max-size-time=30000000000 "
            "async-finalize=false start-index=%d location=\"unused.mkv\"",
            g_atomic_int_get(&next_direct_fragment));
    } else {
        g_string_append(launch, "mp4mux name=mux ! filesink name=filesink location=\"unused.mp4\"");
    }

    logger_log(logger, LOGGER_DEBUG, "created Mux pipeline: %s", launch->str);

    renderer->pipeline = gst_parse_launch(launch->str, &error);

    g_string_free(launch, TRUE);
    if (error || !renderer->pipeline) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline error: %s",
                   error ? error->message : "pipeline was not created");
        g_clear_error(&error);
        discard_partial_renderer();
        g_string_free(filename, TRUE);
        mux_session_clean = FALSE;
        return false;
    }

    if (!no_video && !audio_is_alac) {
        renderer->video_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "video_src");
        if (!renderer->video_appsrc) {
            logger_log(logger, LOGGER_ERR, "Mux pipeline has no video input");
            g_string_free(filename, TRUE);
            mux_session_clean = FALSE;
            discard_partial_renderer();
            return false;
        }
        if (renderer->is_h265) {
            video_caps = gst_caps_from_string(h265_caps);
        } else {
            video_caps = gst_caps_from_string(h264_caps);
        }
        g_object_set(renderer->video_appsrc, "caps", video_caps, NULL);
        gst_caps_unref(video_caps);
    }

    if (!no_audio) {
        renderer->audio_appsrc = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "audio_src");
        if (audio_is_alac) {
            audio_caps = gst_caps_from_string(alac_caps);
        } else {
            audio_caps = gst_caps_from_string(aac_eld_caps);
        }
        g_object_set(renderer->audio_appsrc, "caps", audio_caps, NULL);
        gst_caps_unref(audio_caps);	
    }

    renderer->filesink = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "filesink");
    if (renderer->filesink) g_object_set(renderer->filesink, "location", filename->str, NULL);
    if (direct_video_only) {
        GstElement *splitmux = gst_bin_get_by_name(GST_BIN(renderer->pipeline), "mux");
        if (splitmux) {
            g_object_set(splitmux, "location", filename->str, NULL);
            g_signal_connect(splitmux, "format-location", G_CALLBACK(direct_fragment_location), NULL);
            gst_object_unref(splitmux);
        }
    }
    renderer->bus = gst_element_get_bus(renderer->pipeline);
    if (!renderer->bus) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline has no message bus");
        g_string_free(filename, TRUE);
        mux_session_clean = FALSE;
        discard_partial_renderer();
        return false;
    }

    const GstStateChangeReturn state_result = gst_element_set_state(renderer->pipeline, GST_STATE_PLAYING);
    if (state_result == GST_STATE_CHANGE_FAILURE) {
        logger_log(logger, LOGGER_ERR, "Mux pipeline could not enter PLAYING state");
        g_string_free(filename, TRUE);
        mux_session_clean = FALSE;
        discard_partial_renderer();
        return false;
    }
    GBytes *video_config = NULL;
    g_mutex_lock(&video_config_mutex);
    if (cached_video_config && cached_video_is_h265 == renderer->is_h265)
        video_config = g_bytes_ref(cached_video_config);
    g_mutex_unlock(&video_config_mutex);
    if (direct_video_only && video_config && renderer->video_appsrc) {
        gsize size = 0;
        gconstpointer data = g_bytes_get_data(video_config, &size);
        GstBuffer *header = gst_buffer_new_allocate(NULL, size, NULL);
        if (header) {
            gst_buffer_fill(header, 0, data, size);
            GST_BUFFER_PTS(header) = 0;
            GST_BUFFER_DTS(header) = 0;
            const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(renderer->video_appsrc), header);
            if (flow != GST_FLOW_OK) renderer->failed = TRUE;
        }
    }
    if (video_config) g_bytes_unref(video_config);
    logger_log(logger, LOGGER_INFO, "Started recording to: %s", filename->str);
    g_string_free(filename, TRUE);
    if (renderer->failed) mux_session_clean = FALSE;
    return mux_session_clean;
}

static bool process_audio_codec(unsigned char audio_ct) {
    if (no_audio) return true;
    const gboolean requested_alac = (audio_ct == 2);
    if (renderer && renderer->is_alac != requested_alac) {
        logger_log(logger, LOGGER_DEBUG, "Audio codec changed, recreating mux renderer");
        mux_renderer_destroy_internal();
    }
    audio_is_alac = requested_alac;
    return audio_ct != 2 || mux_renderer_start_internal();
}

static bool process_video_codec(bool is_h265) {
    if (renderer && renderer->pipeline && renderer->is_h265 != is_h265) {
        logger_log(logger, LOGGER_DEBUG, "Video codec changed, recreating mux renderer");
        mux_renderer_destroy_internal();
    }
    video_is_h265 = is_h265;
    logger_log(logger, LOGGER_DEBUG, "Mux renderer video codec: h265=%s", is_h265 ? "true" : "false");
    return mux_renderer_start_internal();
}

static bool push_video_internal(const unsigned char *data, int data_len, uint64_t ntp_time) {
    if (no_video) return true;
    if (!renderer || !renderer->pipeline || !renderer->video_appsrc) return false;

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) return false;
    gst_buffer_fill(buffer, 0, data, data_len);

    if (renderer->base_time == GST_CLOCK_TIME_NONE) {
        renderer->base_time = (GstClockTime)ntp_time;
        renderer->first_video_time = (GstClockTime)ntp_time;
    }

    GstClockTime pts = (GstClockTime)ntp_time - renderer->base_time;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(renderer->video_appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        renderer->failed = TRUE;
        return false;
    }
    renderer->video_buffers++;
    return true;
}

static bool push_audio_internal(const unsigned char *data, int data_len, uint64_t ntp_time) {
    if (no_audio) return true;
    if (!renderer || !renderer->pipeline || !renderer->audio_appsrc) return false;

    if (!renderer->audio_started && renderer->first_video_time != GST_CLOCK_TIME_NONE) {
        renderer->audio_started = TRUE;
        renderer->first_audio_time = (GstClockTime)ntp_time;
        if (renderer->first_audio_time > renderer->first_video_time) {
            GstClockTime silence_duration = renderer->first_audio_time - renderer->first_video_time;
            guint64 num_samples = (silence_duration * 44100) / GST_SECOND;
            gsize silence_size = num_samples * 2 * 2;
            GstBuffer *silence_buffer = gst_buffer_new_allocate(NULL, silence_size, NULL);
            if (!silence_buffer) return false;
            GstMapInfo map;
            if (gst_buffer_map(silence_buffer, &map, GST_MAP_WRITE)) {
                memset(map.data, 0, map.size);
                gst_buffer_unmap(silence_buffer, &map);
            }
            GST_BUFFER_PTS(silence_buffer) = 0;
            GST_BUFFER_DTS(silence_buffer) = 0;
            GST_BUFFER_DURATION(silence_buffer) = silence_duration;
            if (gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), silence_buffer) != GST_FLOW_OK)
                return false;
            logger_log(logger, LOGGER_DEBUG, "Inserted %.2f seconds of silence before audio",
                       (double)silence_duration / GST_SECOND);
        }
    }

    GstBuffer *buffer = gst_buffer_new_allocate(NULL, data_len, NULL);
    if (!buffer) return false;
    gst_buffer_fill(buffer, 0, data, data_len);

    if (renderer->base_time == GST_CLOCK_TIME_NONE) renderer->base_time = (GstClockTime)ntp_time;
    GstClockTime pts = (GstClockTime)ntp_time - renderer->base_time;
    GST_BUFFER_PTS(buffer) = pts;
    GST_BUFFER_DTS(buffer) = pts;
    return gst_app_src_push_buffer(GST_APP_SRC(renderer->audio_appsrc), buffer) == GST_FLOW_OK;
}

static bool mux_renderer_stop_internal(bool require_video_data) {
    if (!renderer || !renderer->pipeline)
        return require_video_data ? FALSE : mux_session_clean;

    gboolean clean = !renderer->failed;
    const gboolean has_video_data = no_video || renderer->video_buffers > 0;

    if (renderer->video_appsrc) {
        if (gst_app_src_end_of_stream(GST_APP_SRC(renderer->video_appsrc)) != GST_FLOW_OK)
            clean = FALSE;
    }
    if (renderer->audio_appsrc) {
        if (gst_app_src_end_of_stream(GST_APP_SRC(renderer->audio_appsrc)) != GST_FLOW_OK)
            clean = FALSE;
    }

    GstMessage *msg = gst_bus_timed_pop_filtered(renderer->bus, 5 * GST_SECOND,
        GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (!msg) {
        logger_log(logger, LOGGER_ERR, "Timed out while finalizing the recording");
        clean = FALSE;
    } else {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *error = NULL;
            gchar *debug = NULL;
            gst_message_parse_error(msg, &error, &debug);
            logger_log(logger, LOGGER_ERR, "Recording pipeline failed: %s",
                       error ? error->message : "unknown error");
            g_clear_error(&error);
            g_free(debug);
            clean = FALSE;
        }
        gst_message_unref(msg);
    }

    gst_element_set_state(renderer->pipeline, GST_STATE_NULL);

    if (renderer->video_appsrc) {
        gst_object_unref(renderer->video_appsrc);
        renderer->video_appsrc = NULL;
    }
    if (renderer->audio_appsrc) {
        gst_object_unref(renderer->audio_appsrc);
        renderer->audio_appsrc = NULL;
    }
    if (renderer->filesink) {
        gst_object_unref(renderer->filesink);
        renderer->filesink = NULL;
    }
    gst_object_unref(renderer->bus);
    renderer->bus = NULL;
    gst_object_unref(renderer->pipeline);
    renderer->pipeline = NULL;

    renderer->base_time = GST_CLOCK_TIME_NONE;
    if (require_video_data && (!has_video_data || !direct_output_exists(renderer->first_fragment))) {
        logger_log(logger, LOGGER_ERR, "Recording finalized without a usable video segment");
        clean = FALSE;
    }
    logger_log(logger, LOGGER_INFO, "Stopped recording");
    audio_is_alac = FALSE;
    video_is_h265 = FALSE;
    if (!clean) mux_session_clean = FALSE;
    return mux_session_clean;
}

static void mux_renderer_destroy_internal(void) {
    if (renderer) {
        if (renderer->pipeline) mux_renderer_stop_internal(FALSE);
        g_free(renderer);
        renderer = NULL;
    }
}

static gpointer handoff_worker_main(gpointer unused) {
    (void) unused;
    for (;;) {
        mux_command_t *command = g_async_queue_pop(handoff_queue);
        gboolean result = TRUE;
        switch (command->type) {
        case MUX_COMMAND_VIDEO:
            if (g_atomic_int_get(&test_consumer_delay_ms) > 0)
                g_usleep((gulong) g_atomic_int_get(&test_consumer_delay_ms) * 1000);
            if (g_atomic_int_get(&handoff_failed)) result = TRUE;
            else result = push_video_internal(command->data, command->data_len, command->ntp_time);
            break;
        case MUX_COMMAND_AUDIO:
            if (g_atomic_int_get(&test_consumer_delay_ms) > 0)
                g_usleep((gulong) g_atomic_int_get(&test_consumer_delay_ms) * 1000);
            if (g_atomic_int_get(&handoff_failed)) result = TRUE;
            else result = push_audio_internal(command->data, command->data_len, command->ntp_time);
            break;
        case MUX_COMMAND_VIDEO_CODEC:
            result = process_video_codec(command->is_h265);
            if (result && g_atomic_int_get(&handoff_may_accept))
                g_atomic_int_set(&handoff_accepting, TRUE);
            break;
        case MUX_COMMAND_AUDIO_CODEC:
            result = process_audio_codec(command->audio_ct);
            if (result && renderer && renderer->pipeline &&
                g_atomic_int_get(&handoff_may_accept))
                g_atomic_int_set(&handoff_accepting, TRUE);
            break;
        case MUX_COMMAND_STOP:
            g_atomic_int_set(&handoff_accepting, FALSE);
            result = mux_renderer_stop_internal(!command->internal_stop);
            break;
        case MUX_COMMAND_QUIT:
            result = mux_renderer_stop_internal(command->require_video_data);
            mux_renderer_destroy_internal();
            release_handoff_reservation(command);
            completion_finish(command->completion, result);
            free_command(command);
            return NULL;
        }
        release_handoff_reservation(command);
        if (!result) mark_handoff_failed("Recording mux handoff failed");
        completion_finish(command->completion,
                          result && !g_atomic_int_get(&handoff_failed));
        free_command(command);
    }
}

static gboolean enqueue_owned_command(mux_command_t *command) {
    g_atomic_int_inc(&handoff_users);
    if (g_atomic_int_get(&handoff_shutdown) || !handoff_queue || !handoff_worker) {
        g_atomic_int_add(&handoff_users, -1);
        return FALSE;
    }
    g_async_queue_push(handoff_queue, command);
    g_atomic_int_add(&handoff_users, -1);
    return TRUE;
}

static gboolean enqueue_control_and_wait(mux_command_type_t type, gboolean is_h265,
                                         unsigned char audio_ct) {
    mux_completion_t completion;
    completion_init(&completion);
    mux_command_t *command = g_new0(mux_command_t, 1);
    command->type = type;
    command->is_h265 = is_h265;
    command->audio_ct = audio_ct;
    command->completion = &completion;
    if (!enqueue_owned_command(command)) {
        free_command(command);
        g_cond_clear(&completion.condition);
        g_mutex_clear(&completion.mutex);
        return FALSE;
    }
    return completion_wait(&completion);
}

static gboolean enqueue_control(mux_command_type_t type, gboolean is_h265,
                                unsigned char audio_ct, gboolean internal_stop) {
    g_atomic_int_inc(&handoff_users);
    if (g_atomic_int_get(&handoff_shutdown) || !handoff_queue || !handoff_worker) {
        g_atomic_int_add(&handoff_users, -1);
        return FALSE;
    }
    if (!reserve_handoff(0)) {
        g_atomic_int_add(&handoff_users, -1);
        mark_handoff_failed("Recording stopped because its bounded handoff queue could not accept a control boundary");
        return FALSE;
    }
    mux_command_t *command = g_try_new0(mux_command_t, 1);
    if (!command) {
        g_atomic_int_add(&handoff_items, -1);
        g_atomic_int_add(&handoff_users, -1);
        mark_handoff_failed("Recording stopped because a handoff control boundary could not be allocated");
        return FALSE;
    }
    command->type = type;
    command->is_h265 = is_h265;
    command->audio_ct = audio_ct;
    command->internal_stop = internal_stop;
    command->reserved = TRUE;
    g_async_queue_push(handoff_queue, command);
    g_atomic_int_add(&handoff_users, -1);
    return TRUE;
}

static bool enqueue_data(mux_command_type_t type, unsigned char *data, int data_len,
                         uint64_t ntp_time, bool *accepted) {
    if (accepted) *accepted = false;
    if (!data || data_len <= 0) return true;

    g_atomic_int_inc(&handoff_users);
    if (g_atomic_int_get(&handoff_shutdown) || !g_atomic_int_get(&handoff_accepting) ||
        !handoff_queue || !handoff_worker) {
        g_atomic_int_add(&handoff_users, -1);
        return true;
    }
    if (!reserve_handoff(data_len)) {
        g_atomic_int_add(&handoff_users, -1);
        mark_handoff_failed("Recording stopped accepting media because its bounded handoff queue overflowed");
        return false;
    }

    mux_command_t *command = g_try_new0(mux_command_t, 1);
    guint8 *copy = g_try_malloc((gsize) data_len);
    if (!command || !copy) {
        g_free(command);
        g_free(copy);
        g_atomic_int_add(&handoff_bytes, -data_len);
        g_atomic_int_add(&handoff_items, -1);
        g_atomic_int_add(&handoff_users, -1);
        mark_handoff_failed("Recording stopped accepting media because the handoff copy could not be allocated");
        return false;
    }
    memcpy(copy, data, (gsize) data_len);
    command->type = type;
    command->data = copy;
    command->data_len = data_len;
    command->ntp_time = ntp_time;
    command->reserved = TRUE;
    g_async_queue_push(handoff_queue, command);
    if (accepted) *accepted = true;
    g_atomic_int_add(&handoff_users, -1);
    return true;
}

bool mux_renderer_choose_audio_codec(unsigned char audio_ct) {
    return enqueue_control(MUX_COMMAND_AUDIO_CODEC, FALSE, audio_ct, FALSE);
}

bool mux_renderer_choose_video_codec(bool is_h265) {
    if (g_atomic_int_get(&handoff_failed)) return false;
    g_atomic_int_set(&handoff_may_accept, TRUE);
    return enqueue_control_and_wait(MUX_COMMAND_VIDEO_CODEC, is_h265, 0);
}

bool mux_renderer_queue_video_codec(bool is_h265) {
    g_atomic_int_set(&handoff_may_accept, TRUE);
    return enqueue_control_and_wait(MUX_COMMAND_VIDEO_CODEC, is_h265, 0);
}

bool mux_renderer_queue_stop(void) {
    g_atomic_int_set(&handoff_may_accept, FALSE);
    g_atomic_int_set(&handoff_accepting, FALSE);
    wait_for_handoff_users();
    return enqueue_control(MUX_COMMAND_STOP, FALSE, 0, TRUE);
}

bool mux_renderer_push_video(unsigned char *data, int data_len, uint64_t ntp_time) {
    return mux_renderer_push_video_with_acceptance(data, data_len, ntp_time, NULL);
}

bool mux_renderer_push_video_with_acceptance(unsigned char *data, int data_len,
                                             uint64_t ntp_time, bool *accepted) {
    if (accepted) *accepted = false;
    if (no_video) return true;
    return enqueue_data(MUX_COMMAND_VIDEO, data, data_len, ntp_time, accepted);
}

bool mux_renderer_push_audio(unsigned char *data, int data_len, uint64_t ntp_time) {
    if (no_audio) return true;
    return enqueue_data(MUX_COMMAND_AUDIO, data, data_len, ntp_time, NULL);
}

bool mux_renderer_stop(void) {
    g_atomic_int_set(&handoff_may_accept, FALSE);
    g_atomic_int_set(&handoff_accepting, FALSE);
    wait_for_handoff_users();
    if (!handoff_queue || !handoff_worker || g_atomic_int_get(&handoff_shutdown)) return false;
    const gboolean failed_before_stop = g_atomic_int_get(&handoff_failed);
    const gboolean result = enqueue_control_and_wait(MUX_COMMAND_STOP, FALSE, 0);
    return result && !failed_before_stop;
}

bool mux_renderer_has_failed(void) {
    return g_atomic_int_get(&handoff_failed) != FALSE;
}

void mux_renderer_destroy(void) {
    g_atomic_int_set(&handoff_may_accept, FALSE);
    g_atomic_int_set(&handoff_accepting, FALSE);
    g_atomic_int_set(&handoff_shutdown, TRUE);
    wait_for_handoff_users();

    if (handoff_queue && handoff_worker) {
        mux_completion_t completion;
        completion_init(&completion);
        mux_command_t *command = g_new0(mux_command_t, 1);
        command->type = MUX_COMMAND_QUIT;
        command->require_video_data = FALSE;
        command->completion = &completion;
        g_async_queue_push(handoff_queue, command);
        completion_wait(&completion);
        g_thread_join(handoff_worker);
        handoff_worker = NULL;
        g_async_queue_unref(handoff_queue);
        handoff_queue = NULL;
    } else {
        mux_renderer_destroy_internal();
    }
    g_atomic_int_set(&handoff_items, 0);
    g_atomic_int_set(&handoff_bytes, 0);
    g_free(output_filename);
    output_filename = NULL;
}

void mux_renderer_set_test_handoff_limits(unsigned int max_items, unsigned int max_bytes) {
    g_atomic_int_set(&handoff_max_items,
                     max_items > 0 && max_items <= G_MAXINT ? (gint) max_items
                                                            : DEFAULT_HANDOFF_MAX_ITEMS);
    g_atomic_int_set(&handoff_max_bytes,
                     max_bytes > 0 && max_bytes <= G_MAXINT ? (gint) max_bytes
                                                            : DEFAULT_HANDOFF_MAX_BYTES);
}

void mux_renderer_set_test_consumer_delay(unsigned int delay_ms) {
    g_atomic_int_set(&test_consumer_delay_ms,
                     delay_ms <= G_MAXINT ? (gint) delay_ms : G_MAXINT);
}
