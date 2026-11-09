// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVENC user client ABI, v3 (kernel: TegraNVENCUserClient.cpp; client: SwitchVideoEncoders/AVE).
//
// The service is "TegraNVENC" (IOServiceMatching), published by default once NVENC answers (E3). Its user
// client's class is named AppleAVE2UserClient, the client class of iPad7,1's own encoder (AppleAVE2Driver, not loaded
// here), which 21H461's media sandbox profiles admit; the processes known to encode are granted outright as well
// (NativeSandboxGrantProcess on the service). Open type TEGRA_NVENC_OPEN_TYPE.
//
// Calls: IOConnectCallStructMethod on any selector the sandbox admits, input {TEGRA_NVENC_TUNNEL_MAGIC, method,
// record...}, output the method's record. Method TEGRA_NVENC_TUNNEL_PROBE only proves a selector is admitted.
//
// Model (the TegraNVDEC model, for an encoder):
//   * NVENC sits behind the MC SMMU in an address space of its own. A buffer is any RAM pages, mapped into that space
//     when allocated and into the allocating task by BufferAlloc itself (cacheable; the reply gives the address), as
//     IOSurface maps its surfaces: 24B119's videocodecd sandbox denies io_connect_map_memory_into_task (KERN_DENIED,
//     its kobject MIG filter). User space never sees an NVENC address: every address method is a relocation (handle +
//     offset) the kernel resolves and bounds-checks against what the firmware may touch there.
//   * The kernel builds the host1x stream itself: only whitelisted NVENC methods, written through the engine's THI
//     METHOD0/METHOD1 pair, then SET_IN_DRV_PIC_SETUP (a kernel-only copy of the checked setup) and EXECUTE, then an
//     OP_DONE syncpoint increment. The address methods of features the checker keeps off (CEA hints, colocated
//     input, QP maps, RC data, partitions) are 0 on every job, as NVIDIA's T210 driver leaves them, so no job keeps
//     another client's address in them; the RC process state is a kernel scratch buffer.
//   * Cache maintenance is explicit: Submit cleans the ranges the CPU wrote (the input picture), Wait invalidates the
//     ranges the CPU will read (status, bitstream). Reference pictures are never touched by the CPU.
//   * Jobs run in submission order; a fence is a syncpoint threshold, waited on by interrupt. The kernel bounds every
//     wait itself (timeout_us is ignored); once published, a job past that bound is a hardware fault and panics.
//
// TegraNVENCInfo says how the firmware reads the input picture, how it addresses chroma planes, and whether P pictures
// are on.
#ifndef TEGRA_NVENC_ABI_H
#define TEGRA_NVENC_ABI_H
#include <stdint.h>

#define TEGRA_NVENC_ABI_VERSION  3
#define TEGRA_NVENC_OPEN_TYPE    0x4e45       // 'NE'
#define TEGRA_NVENC_TUNNEL_MAGIC 0x4e45544eu  // 'NETN'
#define TEGRA_NVENC_TUNNEL_PROBE 0xffffffffu

enum TegraNVENCMethod {
    TegraNVENCMethodGetInfo = 1,      // -> TegraNVENCInfo
    TegraNVENCMethodBufferAlloc = 2,  // TegraNVENCBufferAlloc -> TegraNVENCBufferReply
    TegraNVENCMethodBufferFree = 3,   // TegraNVENCBuffer -> (waits for the last job, revokes the client's mappings)
    TegraNVENCMethodSubmit = 4,       // TegraNVENCSubmit -> TegraNVENCFence
    TegraNVENCMethodWait = 5,         // TegraNVENCWait ->
};

enum {
    TEGRA_NVENC_CODEC_H264 = 1,
    TEGRA_NVENC_CODEC_H265 = 3,
};  // NVENC application ids (clc5b7.h NVC5B7_SET_APPLICATION_ID_ID_NVENC_*)

// Input picture layouts (nvenc_h264_surface_cfg_s of input_cfg); TegraNVENCInfo.input_layout names the one in use.
enum {
    TEGRA_NVENC_LAYOUT_PITCH = 0,    // pitch linear: block_height 0, tiled_16x16 0
    TEGRA_NVENC_LAYOUT_BL2_LOG = 1,  // Tegra block linear, 64Bx8 GOBs two to a block; block_height 1 (log2 GOBs)
    TEGRA_NVENC_LAYOUT_BL2_RAW = 2,  // the same layout; block_height 2 (GOBs)
    TEGRA_NVENC_LAYOUT_BL4_LOG = 3,  // block linear, four GOBs to a block; block_height 2 (log2 GOBs)
    TEGRA_NVENC_LAYOUT_BL1 = 4,      // block linear, one GOB to a block; block_height 0
    TEGRA_NVENC_LAYOUT_TILED16 = 5,  // 16x16 tiles in raster order, tile rows `pitch` x 16 apart; tiled_16x16 1
    TEGRA_NVENC_LAYOUTS = 6,
};

// How a picture's chroma plane is addressed (input, reconstruction, references). Both place chroma `chroma_offset`
// bytes after luma inside one buffer and keep every reading of the methods inside it:
//   OFFSET: the surface config's chroma offset is chroma_offset / 256 and the CHROMA method gets the luma address;
//   METHOD: the CHROMA method gets the chroma address and the surface config's chroma offset is 0.
enum {
    TEGRA_NVENC_CHROMA_OFFSET = 0,
    TEGRA_NVENC_CHROMA_METHOD = 1
};

typedef struct {
    uint32_t abi_version, nvenc_version;  // nvenc_version 0x50: NVENC 5.0 (T210)
    uint32_t codecs;                      // bit n: TEGRA_NVENC_CODEC_* n is accepted by Submit
    uint32_t max_width, max_height, max_mbs;
    uint32_t input_layout;  // TEGRA_NVENC_LAYOUT_*
    uint32_t chroma_mode;   // TEGRA_NVENC_CHROMA_*
    uint32_t p_frames;      // 1: P pictures predicted from NVENC's own reconstruction; 0: intra only
    uint32_t reserved;
    uint64_t memory_limit, memory_free;  // what every session's buffers together may wire, and what is left
} TegraNVENCInfo;

typedef struct {
    uint64_t size;
} TegraNVENCBufferAlloc;

typedef struct {
    uint32_t handle, reserved;
    uint64_t size;     // rounded to 64 KiB
    uint64_t address;  // the buffer in the calling task, until BufferFree or the connection closes
} TegraNVENCBufferReply;

typedef struct {
    uint32_t handle, reserved;
} TegraNVENCBuffer;

typedef struct {
    uint32_t handle, offset, length, reserved;
} TegraNVENCRange;

// One NVENC method. kind 0: `value` is written as is. kind 1: relocation, the method gets
// (NVENC's address of buffer `value` + `offset`) >> 8, so `offset` must be 256-byte aligned.
typedef struct {
    uint16_t method;  // byte offset in the NVENC class (clc5b7.h)
    uint16_t kind;
    uint32_t value, offset;
} TegraNVENCEntry;

enum {
    TEGRA_NVENC_ENTRY_VALUE = 0,
    TEGRA_NVENC_ENTRY_RELOC = 1
};

// The setup is nvenc_h264_drv_pic_setup_s (H.264) or nvenc_h265_drv_pic_setup_s (H.265) followed by the control
// arrays its pic_control offsets name (slice, ME, MD, quantisation), all inside setup_bytes.
#define TEGRA_NVENC_MAX_ENTRIES 48
#define TEGRA_NVENC_MAX_RANGES  4
#define TEGRA_NVENC_MAX_SETUP   2048

typedef struct {
    uint32_t codec;  // TEGRA_NVENC_CODEC_*
    uint32_t entry_count, clean_count;
    uint32_t setup_bytes;
    TegraNVENCEntry entries[TEGRA_NVENC_MAX_ENTRIES];  // no SET_IN_DRV_PIC_SETUP: the kernel adds it
    TegraNVENCRange clean[TEGRA_NVENC_MAX_RANGES];
    uint8_t setup[TEGRA_NVENC_MAX_SETUP];
} TegraNVENCSubmit;

typedef struct {
    uint32_t fence, reserved;
} TegraNVENCFence;

typedef struct {
    uint32_t fence, timeout_us;  // timeout_us: ignored (the kernel bounds every wait, see above)
    uint32_t invalidate_count, reserved;
    TegraNVENCRange invalidate[TEGRA_NVENC_MAX_RANGES];
} TegraNVENCWait;

#ifdef __cplusplus
static_assert(sizeof(TegraNVENCEntry) == 12, "entry");
static_assert(sizeof(TegraNVENCSubmit) == 16 + 12 * 48 + 16 * 4 + 2048, "submit");
static_assert(sizeof(TegraNVENCSubmit) + 8 <= 4096, "submit travels inline");
#else
_Static_assert(sizeof(TegraNVENCSubmit) + 8 <= 4096, "submit travels inline");
#endif
#endif
