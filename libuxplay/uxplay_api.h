#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum uxplay_event_type {
    UXPLAY_EVENT_ENGINE_READY = 0,
    UXPLAY_EVENT_CLIENT_CONNECTING = 1,
    UXPLAY_EVENT_MIRRORING_STARTED = 2,
    UXPLAY_EVENT_STREAM_STOPPED = 3,
    UXPLAY_EVENT_PIN_REQUIRED = 4,
    UXPLAY_EVENT_WARNING = 5,
    UXPLAY_EVENT_ERROR = 6
} uxplay_event_type;

typedef struct uxplay_event {
    uxplay_event_type type;
    const char *device_name;
    const char *device_model;
    const char *device_id;
    const char *message;
    int width;
    int height;
} uxplay_event;

typedef void (*uxplay_event_callback)(const uxplay_event *event, void *context);
typedef void (*uxplay_preview_callback)(const unsigned char *data, int width, int height,
                                        int stride, void *context);

int start_uxplay(int argc, char *argv[]);
void stop_uxplay();
void uxplay_set_video_window(uintptr_t window_handle);
void uxplay_set_event_callback(uxplay_event_callback callback, void *context);
void uxplay_set_preview_callback(uxplay_preview_callback callback, void *context);
int uxplay_start_recording(const char *directory);
int uxplay_stop_recording();
void uxplay_set_recording_test_mode(int enabled);
void uxplay_set_recording_test_stop_result(int clean);

#ifdef __cplusplus
}
#endif
