// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test's display.x1.landscape_video as it calls the kernel (debug kernels): TegraVIC's kernel-test selector, the one
// IOKit path the app's sandbox admits, takes requests with this magic. FORCE holds traits (SwitchDisplayTraits.h) for
// the calling connection until it closes; SAMPLE has the display record its next frame ends (DC A's CRC, the mode,
// what scanned and what it cost); QUERY returns the last sample. A release kernel answers kIOReturnNotPermitted. C.
#pragma once
#include <stdint.h>

#define SWITCH_DISPLAY_LANDSCAPE_RUN_MAGIC  0x444c5356u  // 'DLSV'
#define SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES 240

// Who asked TegraVIC for a job: Core Animation's display server (backboardd: the route under test), the HW Test process
// making these requests, anyone else.
#define SWITCH_DISPLAY_LANDSCAPE_ORIGIN_DISPLAY_SERVER 0
#define SWITCH_DISPLAY_LANDSCAPE_ORIGIN_TEST           1
#define SWITCH_DISPLAY_LANDSCAPE_ORIGIN_OTHER          2

#define SWITCH_DISPLAY_TRAIT_OVERLAY_BLOCK_YUV 1u
#define SWITCH_DISPLAY_TRAIT_COLUMN_SCAN_YUV   2u
#define SWITCH_DISPLAY_TRAIT_OFFSET_CROP       4u

enum {
    SWITCH_DISPLAY_LANDSCAPE_FORCE = 1,   // value: SWITCH_DISPLAY_TRAIT_* (0 drops them)
    SWITCH_DISPLAY_LANDSCAPE_SAMPLE = 2,  // value: frame ends to record, at most SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES
    SWITCH_DISPLAY_LANDSCAPE_QUERY = 3,
};

enum {
    SWITCH_DISPLAY_LANDSCAPE_IDLE = 0,
    SWITCH_DISPLAY_LANDSCAPE_SAMPLING = 1,
    SWITCH_DISPLAY_LANDSCAPE_DONE = 2,
};

// Per recorded frame end: the UI on window B (landscape), a change of mode out, a composition latched.
#define SWITCH_DISPLAY_LANDSCAPE_FRAME_LANDSCAPE 1u
#define SWITCH_DISPLAY_LANDSCAPE_FRAME_SWITCHING 2u
#define SWITCH_DISPLAY_LANDSCAPE_FRAME_OVERLAY   4u

typedef struct {
    uint32_t magic;
    uint32_t op;
    uint32_t value;
    uint32_t reserved;
} SwitchDisplayLandscapeRequest;

typedef struct {
    uint32_t magic;
    uint32_t state;
    uint32_t frames;            // frame ends recorded
    int32_t error;              // why a SAMPLE did not start (an errno), 0 once it runs
    uint32_t traits;            // in force at the last frame end
    uint32_t switches;          // changes of mode latched
    uint32_t overlayLatches;    // compositions latched (video frames shown)
    uint32_t uiLatches;         // UI flips latched
    uint32_t underflows[3];     // frame ends with windows A, B, C underflowing
    uint32_t vicJobs[3];        // VIC jobs submitted, by SWITCH_DISPLAY_LANDSCAPE_ORIGIN_*
    uint32_t vicTurns[3];       // quarter turns recorded instead of copied, by origin
    uint32_t vicEventTurns[3];  // quarter turns whose request carries events, by origin
    uint64_t displayNs;         // CPU in the display's kicks and frame ends
    uint64_t vicNs;             // CPU in TegraVIC's requests and engine thread
    uint32_t crc[SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES];
    uint8_t mode[SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES];
} SwitchDisplayLandscapeReply;

#ifdef __cplusplus
static_assert(sizeof(SwitchDisplayLandscapeReply) ==
                  96 + 4 * SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES + SWITCH_DISPLAY_LANDSCAPE_RUN_FRAMES,
              "the app reads the reply at these offsets");
#endif
