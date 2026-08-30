#ifndef MIRROR_RECOVERY_H
#define MIRROR_RECOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct mirror_resume_gate_s {
    bool awaiting_keyframe;
} mirror_resume_gate_t;

static inline void mirror_resume_gate_begin(mirror_resume_gate_t *gate) {
    if (gate) {
        gate->awaiting_keyframe = true;
    }
}

static inline bool mirror_packet_is_keyframe(const uint8_t *header, size_t length) {
    return header && length >= 6 && header[4] == 0x00 && header[5] == 0x10;
}

static inline bool mirror_resume_gate_should_forward(mirror_resume_gate_t *gate,
                                                     const uint8_t *header,
                                                     size_t length) {
    if (!gate || !gate->awaiting_keyframe) {
        return true;
    }
    if (!mirror_packet_is_keyframe(header, length)) {
        return false;
    }
    gate->awaiting_keyframe = false;
    return true;
}

#endif
