// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decode session's slowest frame (debug builds): each frame's time split at its NVDEC submits and fences into the
// CPU's work before a submit, NVDEC's from a submit to its fence, and placing the picture after the last fence. A
// sample with hidden frames (VP9 superframes) submits more than once; each part adds up across them.
#ifndef AVD_FRAME_TIMING_H
#define AVD_FRAME_TIMING_H
#include <stdint.h>

enum {
    AVD_BEFORE_SUBMIT,
    AVD_SUBMIT_TO_FENCE,
    AVD_FENCE_TO_OUTPUT,
    AVD_FRAME_PARTS
};

typedef struct {
    uint64_t mark;                      // the frame's start, or its last submit or fence
    uint64_t frame[AVD_FRAME_PARTS];    // the frame being decoded
    uint64_t slowest[AVD_FRAME_PARTS];  // the slowest frame the session finished
} AVDFrameTiming;

static inline uint64_t avd_frame_total(const uint64_t parts[AVD_FRAME_PARTS])
{
    return parts[AVD_BEFORE_SUBMIT] + parts[AVD_SUBMIT_TO_FENCE] + parts[AVD_FENCE_TO_OUTPUT];
}

static inline void avd_frame_begin(AVDFrameTiming *t, uint64_t now)
{
    t->mark = now;
    for (unsigned i = 0; i < AVD_FRAME_PARTS; i++)
        t->frame[i] = 0;
}

// The time since the last mark goes to `part`.
static inline void avd_frame_lap(AVDFrameTiming *t, unsigned part, uint64_t now)
{
    t->frame[part] += now - t->mark;
    t->mark = now;
}

static inline void avd_frame_end(AVDFrameTiming *t, uint64_t now)
{
    avd_frame_lap(t, AVD_FENCE_TO_OUTPUT, now);
    if (avd_frame_total(t->frame) <= avd_frame_total(t->slowest))
        return;
    for (unsigned i = 0; i < AVD_FRAME_PARTS; i++)
        t->slowest[i] = t->frame[i];
}

#endif
