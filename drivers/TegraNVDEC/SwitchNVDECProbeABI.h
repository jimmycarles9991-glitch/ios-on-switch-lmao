// SPDX-License-Identifier: BSD-2-Clause-Patent
// The display-buffer probe, debug kernels only (a release kernel answers it as any unknown method, kIOReturnBadArgument):
// a TegraNVDEC call in TegraNVDECABI.h's wire form that the AVD plug-in makes on its own connection. Arm makes that
// connection's next Submit, and only that one, an HEVC Main 10 picture in disp_output_mode 2 or 3 with NVDEC's display
// buffer bound (SET_DISPLAY_BUF_LUMA_OFFSET and SET_DISPLAY_BUF_CHROMA_OFFSET relocations, SwitchNVDECDisplayProbe.h);
// the kernel logs one line saying whether it was queued. Every other Submit, on any connection, is checked as before.
// Kept out of TegraNVDECABI.h: the method number is far from the client's.
#ifndef SWITCH_NVDEC_PROBE_ABI_H
#define SWITCH_NVDEC_PROBE_ABI_H
#include <stdint.h>

enum {
    SwitchNVDECMethodDisplayProbe = 0x4450,  // 'DP'
    SwitchNVDECDisplayProbeArm = 1,
    SwitchNVDECMethodBufferStats = 0x4253,  // 'BS': no input -> SwitchNVDECBufferStats
};

typedef struct {
    uint32_t op, reserved;  // op: SwitchNVDECDisplayProbeArm; no reply
} SwitchNVDECDisplayProbe;

// The connection's buffers since it opened (SwitchNVDECPark.h), for hw_test_app's decode.native_export* through the AVD
// plug-in's SwitchPictureRoutes.
typedef struct {
    uint64_t recycles;        // BufferRecycle calls
    uint64_t replaced;        // of them, one whose buffer an export or the display still held
    uint64_t parked;          // of those, a parked buffer taken back
    uint64_t allocated;       // of those, a new buffer
    uint64_t exports;         // native exports published
    uint64_t export_objects;  // of them, one that needed a new surface object
    uint32_t parked_most, reserved;
} SwitchNVDECBufferStats;

#endif
