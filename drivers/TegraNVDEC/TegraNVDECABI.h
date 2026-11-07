// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDEC user client ABI, v8 (kernel: TegraNVDECUserClient.cpp; client: SwitchVideoDecoders/AVD).
//
// The service is "TegraNVDEC" (IOServiceMatching), published by default once NVDEC has proven itself (N4). Its user
// client's class is named AppleD5500UserClient, iPad7,1's own decoder client (VXD393), which some of 21H461's sandbox
// profiles may admit; the processes known to decode are granted outright (NativeSandboxGrantProcess on the service).
// Open type TEGRA_NVDEC_OPEN_TYPE.
//
// Calls: IOConnectCallStructMethod on any selector the sandbox admits, input {TEGRA_NVDEC_TUNNEL_MAGIC, method,
// record...}, output the method's record. Method TEGRA_NVDEC_TUNNEL_PROBE only proves a selector is admitted.
//
// Model (Linux tegra-drm channel submit / L4T nvhost submit, reduced to what a decoder needs):
//   * NVDEC sits behind the MC SMMU in an address space of its own. A buffer is any RAM pages, mapped into that space
//     when allocated; user space maps it too (IOConnectMapMemory64 with type = handle, cacheable) and never sees an
//     address. Every address method is a relocation (handle + offset) the kernel resolves and bounds-checks.
//   * The kernel builds the host1x stream itself: only whitelisted NVDEC methods, written through the engine's THI
//     METHOD0/METHOD1 pair, followed by an OP_DONE syncpoint increment. The picture setup travels inline and is copied
//     into kernel-only memory after its sizes are checked against the buffers it makes NVDEC write.
//   * Cache maintenance is explicit: Submit cleans the ranges the CPU wrote (bitstream, slice offsets), Wait
//     invalidates the ranges the CPU will read (status, the decoded surface). Surfaces are never written by the CPU.
//   * Present (v6, TegraNVDECInfo.features TEGRA_NVDEC_FEATURE_PRESENT): VIC copies a decoded surface (block linear)
//     into an IOSurface of the calling task as pitch-linear NV12 (drivers/TegraVIC/TegraVICPresent.h), so the CPU
//     neither de-swizzles nor reads it, and Wait need not invalidate it. A refused present leaves the picture's range
//     invalidated, as Wait would have, so the caller's CPU copy reads what NVDEC wrote.
//   * Jobs run in submission order; a fence is a syncpoint threshold. Wait sleeps on the syncpoint interrupt. Every job
//     runs with NVDEC's GP timer (required by the checker), so the kernel bounds each wait itself; timeout_us is
//     ignored. A job past that bound is a hardware fault and panics.
#ifndef TEGRA_NVDEC_ABI_H
#define TEGRA_NVDEC_ABI_H
#include <stdint.h>

#define TEGRA_NVDEC_ABI_VERSION  8
#define TEGRA_NVDEC_OPEN_TYPE    0x4e44       // 'ND'
#define TEGRA_NVDEC_TUNNEL_MAGIC 0x4e44544eu  // 'NDTN'
#define TEGRA_NVDEC_TUNNEL_PROBE 0xffffffffu

enum TegraNVDECMethod {
    TegraNVDECMethodGetInfo = 1,         // -> TegraNVDECInfo
    TegraNVDECMethodBufferAlloc = 2,     // TegraNVDECBufferAlloc -> TegraNVDECBufferReply
    TegraNVDECMethodBufferFree = 3,      // TegraNVDECBuffer -> (waits for the last job, revokes the client's mappings)
    TegraNVDECMethodSubmit = 4,          // TegraNVDECSubmit -> TegraNVDECFence
    TegraNVDECMethodWait = 5,            // TegraNVDECWait ->
    TegraNVDECMethodClaimSelfTest = 6,   // -> TegraNVDECClaim: claimed = 1 for the one caller that runs the clip check
    TegraNVDECMethodReportSelfTest = 7,  // TegraNVDECReport -> (the claiming connection only)
    TegraNVDECMethodWaitSelfTest = 8,    // -> TegraNVDECClaim: sleeps while the check runs, at most 3 s
    TegraNVDECMethodPresent = 9,         // TegraNVDECPresent -> (the picture is in the IOSurface)
};

#define TEGRA_NVDEC_FEATURE_PRESENT (1u << 0)  // TegraNVDECInfo.features: Present is on (VIC proved block-linear input)

// The userland clip check (AVD's ClipCheck.c): one connection per boot claims it and reports its VP9 and HEVC verdicts;
// until then 'vp09' and 'hvc1' are registered nowhere as NVDEC decoders, and a failure turns that codec off
// (TegraNVDECInfo.codecs loses it, Submit refuses it). HEVC Main 10 has its own verdict: a failure there clears only
// TEGRA_NVDEC_HEVC_MAIN10 (Main 10 streams then decode in software; Main stays on NVDEC).
// A claiming connection that closes before reporting gives the claim back.
enum {
    TEGRA_NVDEC_CHECK_UNCLAIMED = 0,
    TEGRA_NVDEC_CHECK_RUNNING = 1,
    TEGRA_NVDEC_CHECK_PASSED = 2,
    TEGRA_NVDEC_CHECK_FAILED = 3,
};

// NVDEC application ids (clc5b0.h NVC5B0_SET_APPLICATION_ID_ID_*). TegraNVDECInfo.codecs has bit n set for each
// codec whose boot self-test (N3) passed; Submit refuses the others.
enum {
    TEGRA_NVDEC_CODEC_H264 = 3,
    TEGRA_NVDEC_CODEC_HEVC = 7,
    TEGRA_NVDEC_CODEC_VP9 = 9
};

// Not an application id: TegraNVDECInfo.codecs bit 16, HEVC Main 10 written as 8-bit NV12 (disp_output_mode 2/3) is
// proven (set with HEVC at N3, which decodes an 8-bit picture; cleared by a failed Main 10 clip check).
#define TEGRA_NVDEC_HEVC_MAIN10 (1u << 16)

typedef struct {
    uint32_t abi_version, nvdec_version;  // nvdec_version 0x20: NVDEC 2.0 (T210)
    uint32_t codecs;                      // bit n: TEGRA_NVDEC_CODEC_* n is accepted by Submit
    uint32_t max_width, max_height;
    uint32_t features;                   // TEGRA_NVDEC_FEATURE_*
    uint64_t memory_limit, memory_free;  // what every session's buffers together may wire, and what is left
} TegraNVDECInfo;

typedef struct {
    uint64_t size;
} TegraNVDECBufferAlloc;

typedef struct {
    uint32_t handle, reserved;
    uint64_t size;
} TegraNVDECBufferReply;  // size rounded to 64 KiB

typedef struct {
    uint32_t handle, reserved;
} TegraNVDECBuffer;

typedef struct {
    uint32_t handle, offset, length, reserved;
} TegraNVDECRange;

// One NVDEC method. kind 0: `value` is written as is. kind 1: relocation, the method gets
// (NVDEC's address of buffer `value` + `offset`) >> 8, so `offset` must be 256-byte aligned.
typedef struct {
    uint16_t method;  // byte offset in the NVDEC class (clc5b0.h)
    uint16_t kind;
    uint32_t value, offset;
} TegraNVDECEntry;

enum {
    TEGRA_NVDEC_ENTRY_VALUE = 0,
    TEGRA_NVDEC_ENTRY_RELOC = 1
};

#define TEGRA_NVDEC_MAX_ENTRIES 64
#define TEGRA_NVDEC_MAX_RANGES  4
#define TEGRA_NVDEC_MAX_SETUP   1024

typedef struct {
    uint32_t codec;  // TEGRA_NVDEC_CODEC_*
    uint32_t entry_count, clean_count;
    uint32_t setup_bytes;                              // sizeof(nvdec_h264_pic_s) / sizeof(nvdec_vp9_pic_s)
    TegraNVDECEntry entries[TEGRA_NVDEC_MAX_ENTRIES];  // no SET_DRV_PIC_SETUP_OFFSET: the kernel adds it
    TegraNVDECRange clean[TEGRA_NVDEC_MAX_RANGES];
    uint8_t setup[TEGRA_NVDEC_MAX_SETUP];
} TegraNVDECSubmit;

typedef struct {
    uint32_t fence, reserved;
} TegraNVDECFence;

typedef struct {
    uint32_t claimed, state;  // state: TEGRA_NVDEC_CHECK_*
} TegraNVDECClaim;

typedef struct {
    uint32_t vp9_ok, hevc_ok, hevc10_ok, reserved;
} TegraNVDECReport;

// A decoded picture of buffer `handle` (NV12 block linear, GOB height 2^block_height_log2: NVDEC's is 1) into the
// '420v'/'420f' IOSurface `surface_id` of the calling task: the window (crop_x, crop_y, width x height) at (dst_x,
// dst_y), clipped to the surface (a grid image's tile goes to its place in the canvas; dst_x a multiple of 256 bytes). pitch: whole 64-byte GOBs; rows: whole pairs of 16-row blocks; chroma at chroma_offset
// (256-byte aligned, past the luma), rows / 2 rows. kIOReturnUnsupported: VIC does not take it (one kernel line says
// why); kIOReturnBadArgument: geometry outside the buffer or an unknown surface.
typedef struct {
    uint32_t handle, surface_id;
    uint32_t pitch, rows, chroma_offset, block_height_log2;
    uint32_t crop_x, crop_y, width, height;
    uint32_t dst_x, dst_y;
} TegraNVDECPresent;

typedef struct {
    uint32_t fence, timeout_us;  // timeout_us: ignored (the kernel bounds every wait, see above)
    uint32_t invalidate_count, reserved;
    TegraNVDECRange invalidate[TEGRA_NVDEC_MAX_RANGES];
} TegraNVDECWait;

#ifdef __cplusplus
static_assert(sizeof(TegraNVDECEntry) == 12, "entry");
static_assert(sizeof(TegraNVDECSubmit) == 16 + 12 * 64 + 16 * 4 + 1024, "submit");
static_assert(sizeof(TegraNVDECSubmit) + 8 <= 4096, "submit travels inline");
#endif
#endif
