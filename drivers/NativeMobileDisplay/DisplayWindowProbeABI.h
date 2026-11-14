// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display's window probes as HW Test calls them (debug kernels): TegraVIC's kernel-test selector 6, the one IOKit
// path the app's sandbox admits, hands a request carrying this magic to the display (switch_display_window_probe_call).
// A release kernel answers kIOReturnNotPermitted. C, so TegraVIC includes it as it is.
#pragma once
#include <stdint.h>

#define SWITCH_DISPLAY_WINDOW_PROBE_MAGIC 0x44575052u  // 'DWPR'
#define SWITCH_DISPLAY_WINDOW_PROBE_STEPS 8

enum {
    SWITCH_DISPLAY_WINDOW_PROBE_START = 1,  // claims the display and starts the probe; the reply says why not
    SWITCH_DISPLAY_WINDOW_PROBE_QUERY = 2,  // the last run's result
    // window A's direct apertures against WINDOW_HEADER's copy (SwitchWindowAperture.h): SwitchDisplayWindowApertureReply
    SWITCH_DISPLAY_WINDOW_PROBE_APERTURE = 3,
};

enum {
    SWITCH_DISPLAY_WINDOW_PROBE_IDLE = 0,
    SWITCH_DISPLAY_WINDOW_PROBE_RUNNING = 1,
    SWITCH_DISPLAY_WINDOW_PROBE_DONE = 2,
};

typedef struct {
    uint32_t magic;
    uint32_t op;
    uint32_t probe;  // display_window_probe::Probe
    uint32_t reserved;
} SwitchDisplayWindowProbeRequest;

typedef struct {
    uint32_t crc;
    uint8_t settled, underflows, changes, mismatches;
    uint8_t check, against, reserved[2];
    char name[16];
} SwitchDisplayWindowProbeStep;

typedef struct {
    uint32_t magic;
    uint32_t probe;
    uint32_t state;
    uint32_t verdict;  // display_window_probe::Verdict
    uint32_t steps;
    uint32_t restored;
    uint32_t frames;
    int32_t error;  // the start's refusal (an errno), 0 once it runs
    char why[64];
    SwitchDisplayWindowProbeStep step[SWITCH_DISPLAY_WINDOW_PROBE_STEPS];
} SwitchDisplayWindowProbeReply;

typedef struct {
    uint32_t magic;
    int32_t error;      // why it did not run (an errno), 0 once it ran
    uint32_t words;     // the words checked
    uint32_t aliased;   // a bit per word: written through each path, read back through the other
    uint32_t restored;  // a bit per word: back to its value from before
    uint32_t reserved;
} SwitchDisplayWindowApertureReply;

#ifdef __cplusplus
static_assert(sizeof(SwitchDisplayWindowApertureReply) == 24, "the app's aperture reply size");
static_assert(sizeof(SwitchDisplayWindowProbeStep) == 28, "the app reads the steps at 28-byte strides");
static_assert(sizeof(SwitchDisplayWindowProbeReply) == 320, "the app's reply size");
#endif
