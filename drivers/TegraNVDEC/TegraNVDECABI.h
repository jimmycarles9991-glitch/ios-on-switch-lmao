// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVDEC user client ABI, v12 (kernel: TegraNVDECUserClient.cpp; client: SwitchVideoDecoders/AVD).
//
// The service is "TegraNVDEC" (IOServiceMatching), published by default once NVDEC has proven itself (N3). Its user
// client's class is named AppleD5500UserClient, iPad7,1's own decoder client (VXD393), which some of 21H461's sandbox
// profiles may admit; the processes known to decode are granted outright (NativeSandboxGrantProcess on the service).
// Open type TEGRA_NVDEC_OPEN_TYPE.
//
// Calls: IOConnectCallStructMethod on any selector the sandbox admits, input {TEGRA_NVDEC_TUNNEL_MAGIC, method,
// record...}, output the method's record. Method TEGRA_NVDEC_TUNNEL_PROBE only proves a selector is admitted.
//
// Model (Linux tegra-drm channel submit / L4T nvhost submit, reduced to what a decoder needs):
//   * NVDEC sits behind the MC SMMU in an address space of its own. A buffer is any RAM pages, mapped into that space
//     when allocated and into the allocating task by BufferAlloc itself (cacheable; the reply gives the address), as
//     IOSurface maps its surfaces: 24B119's videocodecd sandbox denies io_connect_map_memory_into_task (KERN_DENIED,
//     its kobject MIG filter). User space never sees an NVDEC address: every address method is a relocation (handle +
//     offset) the kernel resolves and bounds-checks.
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
//   * Completion (v12, TEGRA_NVDEC_FEATURE_COMPLETION): a port registered with IOConnectSetNotificationPort (type
//     SWITCH_NVDEC_COMPLETION_PORT; the IOSurfaceAccelerator route, which the decoding processes already take) gets
//     one IOKit async completion per job submitted after it: result kIOReturnSuccess or the job's failure, then the
//     arguments {fence, error_status, mbs_correctly_decoded}, which the kernel reads from the job's nvdec_status_s
//     once its fence's interrupt retired it. Nothing waits for it.
//   * Queue room: the engine holds TegraNVDECInfo.jobs_in_flight jobs of all connections. A Submit that finds them all
//     taken sleeps until the oldest retires (SWITCH_NVDEC_FENCE_WAITED_FOR_ROOM), so when a connection's Submit
//     returns, its job jobs_in_flight submits back has ended: a client cycling jobs_in_flight + 1 input buffers never
//     writes one a job still reads, with no wait of its own.
#ifndef TEGRA_NVDEC_ABI_H
#define TEGRA_NVDEC_ABI_H
#include <stdint.h>

#define TEGRA_NVDEC_ABI_VERSION  12
#define TEGRA_NVDEC_OPEN_TYPE    0x4e44       // 'ND'
#define TEGRA_NVDEC_TUNNEL_MAGIC 0x4e44544eu  // 'NDTN'
#define TEGRA_NVDEC_TUNNEL_PROBE 0xffffffffu

enum TegraNVDECMethod {
    TegraNVDECMethodGetInfo = 1,         // -> TegraNVDECInfo
    TegraNVDECMethodBufferAlloc = 2,     // TegraNVDECBufferAlloc -> TegraNVDECBufferReply
    TegraNVDECMethodBufferFree = 3,      // TegraNVDECBuffer -> (waits for the connection's jobs while one uses it)
    TegraNVDECMethodSubmit = 4,          // TegraNVDECSubmit -> TegraNVDECFence
    TegraNVDECMethodWait = 5,            // TegraNVDECWait ->
    TegraNVDECMethodPresent = 6,         // TegraNVDECPresent -> (the picture is in the IOSurface)
    SwitchNVDECMethodBufferRecycle = 7,  // TegraNVDECBuffer -> TegraNVDECBufferReply
    SwitchNVDECMethodExport = 8,         // TegraNVDECPresent -> (leased native NV12 backing)
};

#define SWITCH_NVDEC_FEATURE_NATIVE_OUTPUT (1u << 1)
#define TEGRA_NVDEC_FEATURE_PRESENT        (1u << 0)  // TegraNVDECInfo.features: Present is on (VIC reads block linear)
#define TEGRA_NVDEC_FEATURE_COMPLETION     (1u << 2)
#define SWITCH_NVDEC_COMPLETION_PORT       0x4e43  // 'NC': IOConnectSetNotificationPort's type for job completions
#define SWITCH_NVDEC_COMPLETION_ARGS       3       // fence, error_status, mbs_correctly_decoded

// Service properties, true for each codec in TegraNVDECInfo.codecs: a process that may not open a client finds them by
// matching (IOPropertyMatch), which its sandbox does not filter as it filters property reads.
#define TEGRA_NVDEC_DECODES_H264  "TegraNVDECDecodesH264"
#define TEGRA_NVDEC_DECODES_VP9   "TegraNVDECDecodesVP9"
#define TEGRA_NVDEC_DECODES_HEVC  "TegraNVDECDecodesHEVC"
#define TEGRA_NVDEC_DECODES_MPEG4 "TegraNVDECDecodesMPEG4"
#define TEGRA_NVDEC_DECODES_MPEG2 "TegraNVDECDecodesMPEG2"  // MPEG-1 too

// NVDEC application ids (clc5b0.h NVC5B0_SET_APPLICATION_ID_ID_*). TegraNVDECInfo.codecs has bit n set for each
// codec NVDEC decodes; Submit refuses the others.
enum {
    TEGRA_NVDEC_CODEC_MPEG12 = 1,  // CONTROL_PARAMS codec type MPEG1 (0) or MPEG2 (1)
    TEGRA_NVDEC_CODEC_H264 = 3,
    TEGRA_NVDEC_CODEC_MPEG4 = 4,
    TEGRA_NVDEC_CODEC_HEVC = 7,
    TEGRA_NVDEC_CODEC_VP9 = 9
};

// Not an application id: TegraNVDECInfo.codecs bit 16, HEVC Main 10 is decoded, into 16-bit samples
// (disp_output_mode 1, the 10 bits high: P010). Submit also takes the 10-to-8-bit modes 2 and 3, with or without
// NVDEC's display buffer, every surface bounded as 16-bit; the AVD plug-in uses them only once the display-buffer probe
// says what they write (on the unit, 11-06, mode 2 without a display buffer gave clip T's first picture luma CRC
// 0xeeb22512 where FFmpeg's samples >> 2 give 0xfe5d343b).
#define TEGRA_NVDEC_HEVC_MAIN10 (1u << 16)

typedef struct {
    uint32_t abi_version, nvdec_version;  // nvdec_version 0x20: NVDEC 2.0 (T210)
    uint32_t codecs;                      // bit n: TEGRA_NVDEC_CODEC_* n is accepted by Submit
    uint32_t max_width, max_height;
    uint32_t features;                   // TEGRA_NVDEC_FEATURE_*
    uint32_t jobs_in_flight, reserved;   // the jobs the engine queues (see Queue room above)
    uint64_t memory_limit, memory_free;  // what every session's buffers together may wire, and what is left
} TegraNVDECInfo;

typedef struct {
    uint64_t size;
} TegraNVDECBufferAlloc;

typedef struct {
    uint32_t handle, reserved;
    uint64_t size;     // rounded to 64 KiB
    uint64_t address;  // the buffer in the calling task, until BufferFree or the connection closes
} TegraNVDECBufferReply;

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
    uint32_t setup_bytes;                              // sizeof the codec's nvdec_*_pic_s
    TegraNVDECEntry entries[TEGRA_NVDEC_MAX_ENTRIES];  // no SET_DRV_PIC_SETUP_OFFSET: the kernel adds it
    TegraNVDECRange clean[TEGRA_NVDEC_MAX_RANGES];
    uint8_t setup[TEGRA_NVDEC_MAX_SETUP];
} TegraNVDECSubmit;

typedef struct {
    uint32_t fence, flags;  // SWITCH_NVDEC_FENCE_*
} TegraNVDECFence;

#define SWITCH_NVDEC_FENCE_WAITED_FOR_ROOM (1u << 0)  // the Submit slept until the queue had room

// A decoded picture of buffer `handle` (NV12 block linear, GOB height 2^block_height_log2: NVDEC's is 1) into the
// '420v'/'420f' IOSurface `surface_id` of the calling task, or with 16-bit samples (sample_bytes 2: Main 10's P010)
// into an 'x420'/'xf20' one: the window (crop_x, crop_y, width x height, in samples) at (dst_x, dst_y), clipped to the
// surface (a grid image's tile goes to its place in the canvas; dst_x a multiple of 256 bytes). pitch: whole 64-byte
// GOBs; rows: whole pairs of 16-row blocks; chroma at chroma_offset (256-byte aligned, past the luma), rows / 2 rows.
// plane: the destination plane the picture starts at. A tri-planar 'v0a8' surface (VideoToolbox's HEVC-with-alpha
// target) takes an 8-bit picture at plane 0 into its Y and CbCr planes, or at plane 2 its luma alone into the alpha
// plane; any other surface takes plane 0 only.
// kIOReturnUnsupported: VIC does not take it (one kernel line says why); kIOReturnBadArgument: geometry outside the
// buffer or an unknown surface.
typedef struct {
    uint32_t handle, surface_id;
    uint32_t pitch, rows, chroma_offset, block_height_log2;
    uint32_t crop_x, crop_y, width, height;
    uint32_t dst_x, dst_y;
    uint32_t sample_bytes, plane;
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
