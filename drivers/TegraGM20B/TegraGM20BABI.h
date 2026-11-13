/* SPDX-License-Identifier: MIT */
/* Kernel <-> userland wire ABI for the Tegra X1 GM20B driver (kext: drivers/TegraGM20B, client: Mesa NVK's nvkmd_darwin).
 * Semantics are in scratchpad/gpu/CONTRACT.md. The kext and the client compile this same file; keep it byte-identical in both.
 *
 * Every call is IOConnectCallStructMethod(conn, selector, in, in_size, out, &out_size) with one of the fixed records below.
 * selector = method number, or (tunnel form) any admitted selector with the input prefixed by {TegraGM20BTunnelMagic, method}.
 * All records are little endian, naturally aligned, and at most 4096 bytes (IOKit's inline struct limit), so the kext never
 * sees an out-of-line descriptor. Reserved fields are zero on input and refused otherwise. */
#ifndef TEGRA_GM20B_ABI_H
#define TEGRA_GM20B_ABI_H
#include <stdint.h>

enum {
	TegraGM20BAbiVersion = 1,
	TegraGM20BOpenType = 0x4e56,        /* 'NV': IOServiceOpen type the client passes */
	TegraGM20BTunnelMagic = 0x4e56544e, /* 'NVTN': first word of a tunnelled call */
	TegraGM20BTunnelProbe = 0xffffffff, /* tunnelled method that only answers success */

	TegraGM20BMethodGetInfo = 1,
	TegraGM20BMethodBoAlloc = 2,
	TegraGM20BMethodBoImport = 3,
	TegraGM20BMethodBoFree = 4,
	TegraGM20BMethodVmBind = 5,
	TegraGM20BMethodChannelCreate = 6,
	TegraGM20BMethodChannelDestroy = 7,
	TegraGM20BMethodExec = 8,
	TegraGM20BMethodWaitFence = 9,
	TegraGM20BMethodGetTimestamp = 10,
	TegraGM20BMethodGetStatus = 11,
	TegraGM20BMethodMap = 12,           /* with TegraGM20BInfoKernelMap */
	TegraGM20BMethodUnmap = 13,         /* with TegraGM20BInfoKernelMap */
	TegraGM20BMethodOrder = 14,         /* with TegraGM20BInfoOrder */
	TegraGM20BMethodSurfaceWrites = 15, /* with TegraGM20BInfoSurfaceWrites */
	TegraGM20BMethodSurfaceLayout = 16, /* with TegraGM20BInfoSurfaceLayout */
	TegraGM20BMethodExecSurfaces = 17,  /* with TegraGM20BInfoExecSurfaces */
	TegraGM20BMethodPriority = 18,      /* with TegraGM20BInfoPriority */
	TegraGM20BMethodZbc = 19,           /* with TegraGM20BInfoCompression */
	TegraGM20BMethodSubmit = 20,        /* with TegraGM20BInfoSubmit */
	TegraGM20BMethodCpuAcquire = 21,
	TegraGM20BMethodCpuRelease = 22,
	TegraGM20BMethodSubmitAddress = 23,
	TegraGM20BMethodSurfaceLayoutInfo = 24,
    TegraGM20BMethodNativeSurfaceView = 25,
    TegraGM20BMethodEventImport = 26,
    TegraGM20BMethodEventRelease = 27,
    TegraGM20BMethodEventObserve = 28,
    TegraGM20BMethodResourceObserve = 29,
    TegraGM20BMethodCpuComplete = 30,

	TegraGM20BMaxBindOps = 100,
	TegraGM20BMaxPushes = 255,
	TegraGM20BMaxExecSurfaces = 64,
	TegraGM20BMaxSubmitBytes = 4088, /* leaves room for the tunnel header */
	TegraGM20BMaxJobPushes = 1024,
	TegraGM20BMaxJobUses = 1024,
	TegraGM20BMaxJobFences = 128,
    TegraGM20BMaxJobEvents = 32,
    TegraGM20BMaxJobDependencies = TegraGM20BMaxJobUses + TegraGM20BMaxJobEvents,
};

/* TegraGM20BAbiInfo.flags */
enum {
	TegraGM20BInfoSparse = 1u << 0,     /* VmBind accepts MapSparse */
	TegraGM20BInfoHostImport = 1u << 1, /* BoImport is implemented */
	TegraGM20BInfoZcull = 1u << 2,      /* zcull[] is valid */
	TegraGM20BInfoKernelMap = 1u << 3,  /* Map and Unmap (methods 12, 13) are implemented */
	TegraGM20BInfoOrder = 1u << 4,      /* Order (method 14) is implemented */
	TegraGM20BInfoSurfaceWrites = 1u
		<< 5, /* BoImport takes TegraGM20BImportGpuReads; method 15 is implemented */
	TegraGM20BInfoSurfaceLayout = 1u << 6, /* method 16 is implemented */
	TegraGM20BInfoExecSurfaces = 1u << 7,  /* ExecSurfaces (method 17) is implemented */
	TegraGM20BInfoPriority = 1u << 8,      /* Priority (method 18) is implemented */
	TegraGM20BInfoCompression = 1u << 9, /* compressible kinds bind; method 19 is implemented */
	TegraGM20BInfoKillRecord = 1u << 10, /* a channel's fence word is a TegraGM20BAbiFence */
	TegraGM20BInfoTiledCache = 1u
		<< 11, /* every GR context holds the tile coalescer's table: a client may enable the tiled cache */
	TegraGM20BInfoSubmit = 1u << 12, /* atomic surface uses and pushes (method 20) */
	TegraGM20BInfoCpuAccess = 1u << 13,
	TegraGM20BInfoSubmitAddress = 1u << 14,
	TegraGM20BInfoCpuPreserveDirty = 1u << 15,
	TegraGM20BInfoCanonicalSurfaceLayout = 1u << 16,
    TegraGM20BInfoPushFences = 1u << 26, /* SubmitAddress takes TegraGM20BPushFence */
};

/* CpuComplete maintains cached CPU ranges only while the accepted channel prefix still owns their current
 * GPU content. Stale intervals are not acknowledged. Pending/failed intervals perform no cache maintenance. */
struct TegraGM20BAbiCpuComplete {
    uint64_t channel_uid, ranges_address;
    uint32_t channel, seqno, range_count, reserved;
};

struct TegraGM20BAbiCpuRange {
    uint64_t handle, offset, bytes;
};

struct TegraGM20BAbiCpuCompleteReply {
    uint32_t current_count, stale, pending, failed;
    int32_t first_error;
    uint32_t reserved;
};

/* This follows AbiFenceSemantic in the validated read-only fence mapping. */
struct TegraGM20BAbiFenceIdentity {
    uint64_t channel_uid;
};

struct TegraGM20BAbiEventImport {
    uint32_t port, reserved;
};

struct TegraGM20BAbiEventImportReply {
    uint64_t handle, currentValue;
};

struct TegraGM20BAbiEventRelease {
    uint64_t handle;
};

enum {
    TegraGM20BEventObserveCancel = 1u << 0,
    TegraGM20BEventObserveProbe = 1u << 1,
    TegraGM20BEventObserveTerminal = 1u << 2,
};

/* Arm uses IOConnectCallAsyncStructMethod, including the ordinary tunnel prefix. Cancel is synchronous and
 * identifies the original (handle,cookie); its one-shot async callback owns the original reference until delivery.
 * Callback payload is cookie; result is Success when resolvable, Aborted on producer failure, or Aborted on cancel.
 * Terminal waits for the selected producer's external semantic result. Probe|Terminal queries that result. */
struct TegraGM20BAbiEventObserve {
    uint64_t handle, value, cookie;
    uint32_t flags, reserved;
};

enum { TegraGM20BResourceObserveCancel = 1u << 0 };

/* Readiness is one-shot and grants no ownership. Cancel acknowledges through the original async callback.
 * uses_address names use_count AbiSurfaceUse records, copied once before resolving retained backing identities. */
struct TegraGM20BAbiResourceObserve {
    uint64_t cookie, uses_address;
    uint32_t use_count, flags;
    uint64_t reserved;
};

struct TegraGM20BAbiSubmitAdmission {
    uint64_t cookie, reserved;
};

struct TegraGM20BAbiEventUse {
    uint64_t handle, value;
};

/* With SubmitHasEvents, this follows the addressed job's surface uses, then waits followed by signals. */
struct TegraGM20BAbiSubmitEvents {
    uint32_t wait_count, signal_count;
    uint64_t reserved;
};

/* Returns an immutable read-only representation of an attested IOSurface BO. handle=0 means this format has no representation view.
 * A native view's generation mismatch at SubmitAddress returns kIOReturnNotReady before publication. */
struct TegraGM20BAbiNativeSurfaceView {
    uint64_t source_handle;
    uint32_t flags, reserved;
};

struct TegraGM20BAbiNativeSurfacePlane {
    uint64_t offset, bytes;
    uint32_t width, height, pitch, bpe, block_height_log2, reserved;
};

struct TegraGM20BAbiNativeSurfaceViewReply {
    uint64_t handle, bytes, representation_generation, allocation_generation;
    uint32_t format, plane_count, kind, reserved;
    uint32_t crop_x, crop_y, crop_width, crop_height;
    struct TegraGM20BAbiNativeSurfacePlane planes[3];
};

enum {
	TegraGM20BCpuRead = 1u << 0,
	TegraGM20BCpuWrite = 1u << 1,
	TegraGM20BCpuAvoidSync = 1u << 2,
	TegraGM20BCpuDiscard = 1u << 3,
	TegraGM20BCpuPreserveDirty = 1u << 6,
};

struct TegraGM20BAbiCpuAccess {
	uint64_t handle, offset, bytes;
	uint32_t access, reserved;
};

/* The addressed record is a TegraGM20BAbiSubmit followed by its pushes and uses. The kernel copies it once before
 * validation. A job's final entry must complete its method packet; the entire job is published together. */
struct TegraGM20BAbiSubmitAddress {
	uint64_t address;
	uint32_t bytes, reserved;
};

/* Method 1, output only. Everything NVK's struct nv_device_info needs that the client cannot derive. */
struct TegraGM20BAbiInfo {
	uint32_t abi_version; /* TegraGM20BAbiVersion */
	uint32_t flags;       /* TegraGM20BInfo* */
	uint16_t chipset;     /* NV_PMC_BOOT_0 implementation: 0x12b */
	uint16_t device_id;   /* nouveau GETPARAM_PCI_DEVICE equivalent; 0 if none */
	uint8_t gpc_count;    /* 1 on T210 */
	uint8_t reserved0;
	uint16_t tpc_count;   /* total TPCs: 2 on T210 */
	uint16_t cls_copy;    /* 0xb0b5 MAXWELL_DMA_COPY_A */
	uint16_t cls_eng2d;   /* 0x902d FERMI_TWOD_A */
	uint16_t cls_eng3d;   /* 0xb197 MAXWELL_B */
	uint16_t cls_m2mf;    /* 0xa140 KEPLER_INLINE_TO_MEMORY_B */
	uint16_t cls_compute; /* 0xb1c0 MAXWELL_COMPUTE_B */
	uint16_t cls_gpfifo;  /* 0xb06f MAXWELL_CHANNEL_GPFIFO_A */
	uint32_t reserved1;
	uint64_t va_start;  /* client-managed GPU VA range [va_start, va_end) */
	uint64_t va_end;    /* the kext keeps everything at and above va_end for itself */
	uint32_t page_size; /* bind granularity: 4096 */
	uint32_t
		big_page_size; /* GMMU big page the kext may use for aligned binds (0x20000), informational */
	uint64_t max_bo_bytes;
	uint32_t zcull[12]; /* layout of drm_nouveau_get_zcull_info / struct nv_zcull_device_info */
	char chip_name[16]; /* "GM20B" */
	char device_name[64]; /* "NVIDIA Tegra X1 (GM20B)" */
};

/* TegraGM20BAbiBoAlloc.flags */
enum {
	TegraGM20BBoCoherent = 1u
		<< 0, /* CPU map is write-combined (Normal non-cacheable); else Normal cacheable */
	TegraGM20BBoContiguous = 1u
		<< 1, /* one physically contiguous span (scanout, firmware-adjacent use) */
	/* Memory for compressible binds (TegraGM20BInfoCompression), alone of the flags: bytes a 128 KiB multiple, 128 KiB-aligned
	 * physically contiguous runs of the kernel's GPU pool under its tiers (kIOReturnNoMemory while they refuse it or no run
	 * is free), never mapped for the CPU (Map refuses its token) */
	TegraGM20BBoCompressible = 1u << 2,
    TegraGM20BBoControl = 1u << 3, /* Coherent command/control allocation; bounded ready-page preference. */
};

/* Method 2. Zero-filled pages below 4 GiB. map_token is the clientMemoryForType type for IOConnectMapMemory64. */
struct TegraGM20BAbiBoAlloc {
	uint64_t bytes;      /* page_size multiple, <= max_bo_bytes */
	uint32_t align_log2; /* 12..21; physical alignment only matters with Contiguous */
	uint32_t flags;      /* TegraGM20BBo* */
};

struct TegraGM20BAbiBoAllocReply {
	uint64_t
		handle; /* nonzero, unique per connection, never reused while the connection lives */
	uint32_t map_token; /* nonzero */
	uint32_t reserved;
};

/* Method 3. Wires [address, address+bytes) of the calling task (an IOSurface's pages) and makes them a BO. No map token: the
 * caller keeps its own mapping. Reply: TegraGM20BAbiBoAllocReply with map_token 0.
 * surface_id: the IOSurface whose memory the range lies in (IOSurfaceGetID), or 0 for other memory. With an ID the kernel
 * keeps the import coherent across every process's IOSurfaceLock/Unlock of that surface (an unlock that wrote cleans it
 * for the GPU, a lock after GPU writes invalidates it for the CPU). Memory without an ID is cleaned once, at import;
 * later CPU writes to it are the caller's to clean.
 * flags: TegraGM20BImportGpuReads says the GPU only reads the surface until a SurfaceWrites (method 15) names it, so a CPU
 * lock of a surface only ever sampled invalidates nothing. Without it every submission may have written the import. */
enum {
	TegraGM20BImportGpuReads = 1u << 0,
};

struct TegraGM20BAbiBoImport {
	uint64_t address; /* page aligned */
	uint64_t bytes;   /* page multiple */
	uint32_t surface_id;
	uint32_t flags; /* TegraGM20BImport* */
};

/* Method 15. The GPU may write the IOSurface surface_id from now on, in any process that imported it: before a submission
 * that renders, blits or stores into it is made. */
struct TegraGM20BAbiSurfaceWrites {
	uint32_t surface_id;
	uint32_t reserved;
};

/* Method 16. The layout of the IOSurface whose import `handle` is (its BoImport named the surface), decided once for every
 * process at its first GPU import that asks, by the shared rule (TegraGM20BLayout.h): block-linear when that importer will
 * write it (TegraGM20BLayoutWrites: a render target, a store, a blit destination), its texels are width x height of bpp
 * bytes from the surface's first byte in rows bytes_per_row apart, the shape tiles, and no other import of it exists;
 * pitch otherwise, for good. A surface becoming block-linear has what it holds tiled before the reply. A later call (any
 * process, any flags) answers the decision. Block-linear surfaces are bound with TGM_LAYOUT_KIND and read as
 * TegraGM20BLayout.h lays them out by the GPU and the display; a CPU lock sees their texels as rows bytes_per_row apart.
 * Reply: TegraGM20BLayoutTiled at the asked shape; 0 pitch; TegraGM20BLayoutOtherShape tiled at another shape. */
enum {
	TegraGM20BLayoutWrites = 1u << 0,
};

enum {
	TegraGM20BLayoutTiled = 1,
	TegraGM20BLayoutOtherShape = 2,
};

struct TegraGM20BAbiSurfaceLayout {
	uint64_t handle;
	uint32_t width, height, bpp, bytes_per_row;
	uint32_t flags; /* TegraGM20BLayout* */
	uint32_t reserved;
};

struct TegraGM20BAbiSurfaceLayoutReply {
	uint32_t layout; /* 0, TegraGM20BLayoutTiled or TegraGM20BLayoutOtherShape */
	uint32_t reserved;
};

struct TegraGM20BAbiSurfaceLayoutInfo {
	uint32_t layout, width, height, bpp;
	uint32_t pitch, y_log2, rows, reserved;
	uint64_t align, bytes;
};

/* Method 4. The handle dies now; its pages are released only after every channel of the connection has completed what it had
 * submitted at the time of this call. Any VA still bound to it is unbound first. */
struct TegraGM20BAbiBoFree {
	uint64_t handle;
};

/* Method 5. Applied in order. An unmap (and a map's unbind of what it overlaps) has invalidated the GPU's L2 and TLB
 * before return; a map is seen by every submission made after the call returns (each of the connection's channels
 * invalidates at its next submission), and by none of the work already submitted. */
enum {
	TegraGM20BBindMap = 1,   /* [va, va+range) -> handle at bo_offset, PTE kind = kind */
	TegraGM20BBindMapVolatile = 4, /* CPU command/control memory bypasses GPU L2; kind must be 0 */
	TegraGM20BBindUnmap = 2, /* [va, va+range) -> invalid (GPU access faults) */
	TegraGM20BBindMapSparse =
		3, /* [va, va+range) -> sparse (reads 0, writes dropped); needs InfoSparse */
};

/* Compression (TegraGM20BInfoCompression): a Map with a compressible kind (one the GMMU kind table maps to another, e.g.
 * C32_2C 0xdb, Z24S8_2CZ) compresses through the GPU's L2; every GPU engine reading or writing through such a mapping
 * sees plain data, the CPU and the display read the compressed bytes. It needs a BO from BoAlloc (never an import)
 * whose memory is in physically contiguous, 128 KiB-aligned runs (Compressible, or Contiguous, Coherent or not, with
 * align_log2 >= 17 and bytes a 128 KiB multiple), and va, bo_offset and range 128 KiB multiples; else kIOReturnBadArgument. The BO's
 * compression lines are its own from its first compressed Map until it is freed, shared by every Map of it: map it with
 * one compressible kind only, since an uncompressed alias reads compressed bytes. kIOReturnNoResources: none were left
 * when it first needed them, and no Map of that BO will compress; bind it uncompressed. */
struct TegraGM20BAbiBindOp {
	uint32_t op;        /* TegraGM20BBind* */
	uint32_t kind;      /* GMMU PTE kind (NVK's pte_kind); 0 = pitch/generic */
	uint64_t handle;    /* Map only */
	uint64_t bo_offset; /* Map only; page multiple */
	uint64_t va;        /* page multiple, inside [va_start, va_end) */
	uint64_t range;     /* page multiple, nonzero */
};

struct TegraGM20BAbiVmBind {
	uint32_t count; /* 1..TegraGM20BMaxBindOps */
	uint32_t reserved;
	struct TegraGM20BAbiBindOp ops[TegraGM20BMaxBindOps];
};

/* TegraGM20BAbiChannelCreate.engines: low bits match NVK's enum nvkmd_engines. */
enum {
	TegraGM20BEngineCopy = 1u << 0,
	TegraGM20BEngine2D = 1u << 1,
	TegraGM20BEngine3D = 1u << 2,
	TegraGM20BEngineM2MF = 1u << 3,
	TegraGM20BEngineCompute = 1u << 4,
    TegraGM20BEngineCopyOnly = 1u << 31, /* Requires exactly EngineCopy. */
};

/* Method 6. A GPFIFO channel on the GR runlist in the connection's VM. EngineCopyOnly binds only CE on subchannel 4;
 * legacy channels bind eng3d, compute, m2mf, eng2d and copy on subchannels 0..4. fence_token maps (read-only) a page whose uint32 at fence_offset is the channel's
 * last completed seqno. */
struct TegraGM20BAbiChannelCreate {
	uint32_t engines;
	uint32_t reserved;
};

struct TegraGM20BAbiChannelCreateReply {
	uint32_t channel;      /* nonzero, per connection */
	uint32_t fence_token;  /* clientMemoryForType type of the fence page */
	uint32_t fence_offset; /* byte offset of this channel's uint32 seqno in that page */
	uint32_t reserved;
};

/* What fence_offset points at (TegraGM20BInfoKillRecord). The GPU writes `completed`. When the kext kills the channel
 * (method 11) it writes the rest, then moves `completed` past every seqno it gave out, so a client that sees `completed`
 * pass a seqno sees the record too, with a load barrier between the two reads. A seqno s with
 * (int32_t)(s - killed_first) >= 0 and (int32_t)(killed_last - s) >= 0 never ran to its end: that work failed with
 * `error`, method 11's code. error 0: the channel lives. ChannelCreate zeroes the record of the channel it makes (a slot's
 * seqnos continue across channels, so a record names the seqnos of the channel it was written for alone). */
struct TegraGM20BAbiFence {
	uint32_t completed;
	uint32_t killed_first;
	uint32_t killed_last;
	uint32_t error;
};

enum {
    TegraGM20BFenceSemanticPending = 1u << 0,
    TegraGM20BFenceSemanticFailed = 1u << 1,
};

/* This immediately follows AbiFence. Read epoch, fields, epoch with acquire ordering;
 * an odd or changed epoch requires WaitFence. Prefix comparisons use signed32 sequence differences. */
struct TegraGM20BAbiFenceSemantic {
    uint32_t epoch, pending_first, failed_first, flags;
};

/* Method 7. Waits (bounded) for the channel to go idle, then tears it down. */
struct TegraGM20BAbiChannel {
	uint32_t channel;
	uint32_t reserved;
};

/* Method 8. Appends one GPFIFO entry per push, in order and contiguously, then the kext's own fence push (wait-for-idle, the
 * cache flush needed for CPU visibility, semaphore release of seqno to the fence page), then rings GP_PUT. count may be 0: no
 * GPU work, the reply is the last seqno already submitted (and errors of the channel are still reported). */
enum {
	TegraGM20BPushNoPrefetch = 1u << 0, /* GPFIFO entry NO_PREFETCH bit */
	/* From this submission on, every writable import of the connection counts as GPU-written, TegraGM20BImportGpuReads
	 * or not: the client could not deliver a SurfaceWrites. Carried by a real push, so it takes effect with the work. */
	TegraGM20BPushImportsWritten = 1u << 1,
	TegraGM20BPushIncomplete = 1u << 2,
	/* SubmitAddress with InfoPushFences: after this push the GPU waits for idle and releases the channel's next seqno
	 * (an internal fence, with its completion interrupt), then runs the next push. The job's own fence releases the
	 * seqno after its last such fence, which the reply names. Never on an Incomplete push or the job's last; at most
	 * TegraGM20BMaxJobFences. */
	TegraGM20BPushFence = 1u << 3,
};

struct TegraGM20BAbiPush {
	uint64_t va;     /* 4-byte aligned GPU VA of the method stream */
	uint32_t dwords; /* 1..(1 << 21) - 1 */
	uint32_t flags;  /* TegraGM20BPush* */
};

struct TegraGM20BAbiExec {
	uint32_t channel;
	uint32_t count; /* 0..TegraGM20BMaxPushes */
	struct TegraGM20BAbiPush pushes[TegraGM20BMaxPushes];
};

struct TegraGM20BAbiExecReply {
	uint32_t seqno; /* fence of this submission; compare with (int32_t)(a - b) >= 0 */
	uint32_t reserved;
};

/* TegraGM20BAbiSurfaceUse.access: one or both */
enum {
	TegraGM20BSurfaceRead = 1u << 0,
	TegraGM20BSurfaceWrite = 1u << 1, /* renders, blits or stores into it */
	TegraGM20BSurfaceHostWritten = 1u << 3, /* SubmitAddress explicit visibility: CPU bytes changed since GPU use */
	TegraGM20BSurfaceGpuAddress = 1u << 2, /* SubmitAddress only: handle is a GPU VA in a live binding */
    TegraGM20BSurfaceRange = 1u << 4, /* With GpuAddress, reserved is the nonzero byte extent. */
};

/* TegraGM20BAbiExecSurfaces.flags */
enum {
	TegraGM20BExecSurfacesAll = 1u
		<< 0, /* it touches more imports than it lists: the Exec runs as one with no list */
};

/* SubmitAddress only. Flags zero preserve the legacy host acquire and external release. Internal execution fences
 * do not expose GPU writes to the CPU or other engines; request ExternalVisibility at those boundaries. */
enum {
	TegraGM20BSubmitVisibilityExplicit = 1u << 1,
	TegraGM20BSubmitExternalVisibility = 1u << 2,
    TegraGM20BSubmitHasEvents = 1u << 3,
    TegraGM20BSubmitAdmissionBegin = 1u << 4,
    TegraGM20BSubmitAdmissionContinue = 1u << 5,
    TegraGM20BSubmitAdmissionEnd = 1u << 6,
    TegraGM20BSubmitAdmissionCancel = 1u << 7,
};

/* Method 17. The host imports (BoImport handles) the channel's next Exec touches, as an Apple command buffer carries its
 * resource list. That Exec starts with a semaphore acquire, as Order's (method 14), on each submission of another channel,
 * in any process, whose own list named a listed import as written and that may still be running, so it reads and writes
 * the surface after them on the GPU, with no CPU wait; and it counts as a GPU write (TegraGM20BImportGpuReads, the
 * IOSurfaceLock wait) of the imports it lists with TegraGM20BSurfaceWrite only. An Exec with no list, or with
 * TegraGM20BExecSurfacesAll, waits for nothing and counts as a write of every writable import of the connection for the
 * IOSurfaceLock wait alone: no Exec waits for it on the GPU. A client's surfaces are fenced from the point it sends lists.
 * The channel's next Exec takes the list whether it succeeds or not: a retry after kIOReturnBusy sends it again. The record
 * may end after its `count` uses, as Exec's after its pushes. A handle that is not a live import is passed over. A channel
 * queues at most TegraGM20BMaxOrderWaits acquires: an Exec that cannot queue all required waits fails before submitting. */
struct TegraGM20BAbiSurfaceUse {
	uint64_t handle;
	uint32_t access; /* TegraGM20BSurface* */
	uint32_t reserved;
};

struct TegraGM20BAbiExecSurfaces {
	uint32_t channel;
	uint32_t count; /* 0..TegraGM20BMaxExecSurfaces */
	uint32_t flags; /* TegraGM20BExecSurfaces* */
	uint32_t reserved;
	struct TegraGM20BAbiSurfaceUse uses[TegraGM20BMaxExecSurfaces];
};

/* Method 20, reply TegraGM20BAbiExecReply. The header is followed by push_count AbiPush records, then surface_count
 * AbiSurfaceUse records. The exact byte count must fit MaxSubmitBytes. The surface list and pushes are consumed together,
 * under the connection lock; malformed records or unresolved dependencies publish no GPU work. No pending method 17 list
 * is used. surface_flags has ExecSurfaces semantics (All for an unlisted submission). */
struct TegraGM20BAbiSubmit {
	uint32_t channel;
	uint32_t push_count;
	uint32_t surface_count;
	uint32_t surface_flags;
};

/* Method 9. Returns kIOReturnSuccess once completed seqno has passed `seqno`, kIOReturnTimeout after timeout_ns (0 = poll once),
 * kIOReturnNotResponding if the channel is dead. */
struct TegraGM20BAbiWaitFence {
	uint32_t channel;
	uint32_t seqno;
	uint64_t timeout_ns;
};

/* Method 10, output only: PTIMER, the clock the GPU writes in timestamp queries. It counts 32 a tick of the 19.2 MHz
 * oscillator, so a unit is 625/384 ns, not one: GK20A-family PTIMERs have no rate registers to make it count nanoseconds. */
struct TegraGM20BAbiTimestamp {
	uint64_t ns;
};

/* Method 11. Input TegraGM20BAbiChannel; error 0 means healthy, anything else means the kext killed the channel (its fence
 * word was moved past every seqno it gave out, so no acquire or wait stays on it). Only that channel is dead: Exec, Order
 * and WaitFence on it answer kIOReturnNotResponding, while the connection's BOs, binds and other channels go on, and
 * ChannelDestroy then ChannelCreate (whose reply names the new channel and fence word) gives the client a new one. */
struct TegraGM20BAbiStatus {
	uint32_t completed;
	uint32_t submitted;
	uint32_t error; /* 0, or 1 MMU fault, 2 GR exception, 3 PBDMA/host error, 4 timeout */
	uint32_t reserved;
	uint64_t fault_va; /* for error 1 */
};

/* TegraGM20BAbiMap.flags */
enum {
	TegraGM20BMapReadOnly = 1u << 0, /* kIOMapReadOnly */
	TegraGM20BMapFixed = 1u
		<< 1, /* kIOMapOverwrite at `address`; else kIOMapAnywhere (address 0) */
};

/* Method 12. Maps the clientMemoryForType type `token` (a BO's map_token, a channel's fence_token) into the calling task, exactly
 * as IOConnectMapMemory64(conn, token, mach_task_self(), &address, &bytes, options) does, but from inside io_connect_method: a
 * sandbox may refuse the MIG routine io_connect_map_memory_into_task (WebKit's GPU process profile in 21H461 admits
 * io_connect_method and not it), as Apple's own GPU drivers map their resources in their external methods. The mapping belongs
 * to the connection: Unmap removes it, and so does closing the connection. */
struct TegraGM20BAbiMap {
	uint32_t token;
	uint32_t flags;   /* TegraGM20BMap* */
	uint64_t address; /* Fixed only; else 0 */
};

struct TegraGM20BAbiMapReply {
	uint64_t address;
	uint64_t bytes; /* the mapping's length (IOConnectMapMemory64's size) */
};

/* Method 13. Removes the mapping method 12 made of `token` at `address` (IOConnectUnmapMemory64's work). */
struct TegraGM20BAbiUnmap {
	uint32_t token;
	uint32_t reserved;
	uint64_t address;
};

/* TegraGM20BAbiOrder.flags: one or both */
enum {
	TegraGM20BOrderAfterTask = 1u << 0,  /* `channel` waits for the task's other channels */
	TegraGM20BOrderBeforeTask = 1u << 1, /* the task's other channels wait for `channel` */
};

enum {
	TegraGM20BMaxOrderWaits = 16, /* acquires one channel's next submission can have queued */
};

/* Method 14. Order on the GPU between the channels of the calling task (its connections: two drivers of one process, e.g.
 * the render server's raw GM20B path and NVK), with no CPU wait. The kext queues semaphore acquires (ACQ_GEQ, acquire switch
 * on: the host runs other channels meanwhile) that a channel's next Exec starts with, ahead of its pushes, on the fence words
 * of the task's other channels (every address space maps the fence pages read-only for them):
 *  - AfterTask: the next submission of `channel` waits for the last submission, as of this call, of every other channel of
 *    the task that has work pending;
 *  - BeforeTask: the next submission of every other channel of the task waits for `channel` to complete `seqno` (0: its
 *    last submission; a seqno not submitted yet is refused).
 * Every acquire names work submitted before the call, so no two submissions can wait for each other. A channel the task makes
 * after the call is not ordered by it; work that completed already queues nothing. reply.waits: the acquires queued.
 * kIOReturnNotReady: an address space has no view of the fence pages; kIOReturnNoResources: a channel has
 * TegraGM20BMaxOrderWaits acquires from other channels queued. Either way the caller orders that switch itself.
 * kIOReturnNotResponding: `channel` was killed (method 11); killed channels of the task are left out. */
struct TegraGM20BAbiOrder {
	uint32_t channel;
	uint32_t flags; /* TegraGM20BOrder* */
	uint32_t seqno; /* BeforeTask */
	uint32_t reserved;
};

/* Method 18. The GPU priority the process's Metal command queues asked for (-[MTLCommandQueue setGPUPriority:] and
 * setBackgroundGPUPriority:), Metal's values, the lower the more urgent: Core Animation's render server asks for 0 (its
 * window server renderer: CA::OGL::new_metal_context's MetalPriority 4), its asynchronous dispatcher 1, its other
 * contexts 2 and 4. It sets the runlist level of every channel of the connection, made before the call or after:
  * 0 the compositor's, the highest, 1 and 2 an app's, 3 and above background. background_priority is the one for while
 * runningboardd denies the process the GPU (task_is_gpu_denied, IOKit's "foreground" privilege). Values above
 * TegraGM20BMaxPriority are refused. */
enum {
	TegraGM20BMaxPriority = 5,
};

struct TegraGM20BAbiPriority {
	uint32_t priority;
	uint32_t background_priority;
};

struct TegraGM20BAbiOrderReply {
	uint32_t waits;
	uint32_t reserved;
};

/* Method 19. A zero-bandwidth-clear entry: a full-tile clear (CLEAR_SURFACE) of a compressed surface to a value in the
 * table writes the tiles' compression state instead of their pixels. The table is the GPU's, shared by every process,
 * 15 colour and 15 depth entries, never emptied: transparent black, opaque white, RGBA32F 0 and 1, opaque black as
 * A8B8G8R8 and A8R8G8B8, and depth 0.0 and 1.0 are in it from boot. Adding an entry that is there answers its index;
 * kIOReturnNoResources: the table is full (clears to that value are ordinary clears).
 * format: the NV9097 ZBC colour format (FERMI_A_ZBC_COLOR_V0_FMT_*), or 1 (FP32) for depth. ds: the value as
 * SET_COLOR_CLEAR_VALUE(0..3) takes it, or SET_Z_CLEAR_VALUE in ds[0]. l2: the value as the surface stores it, the packed
 * pixel or the depth word (l2[0]). */
enum {
	TegraGM20BZbcColor = 1,
	TegraGM20BZbcDepth = 2,
};

struct TegraGM20BAbiZbc {
	uint32_t type; /* TegraGM20BZbc* */
	uint32_t format;
	uint32_t ds[4];
	uint32_t l2[4];
};

struct TegraGM20BAbiZbcReply {
	uint32_t index; /* 1..15 */
	uint32_t reserved;
};

#ifdef __cplusplus
#define TEGRA_GM20B_ASSERT(x, n) static_assert(sizeof(x) == (n), #x " layout")
#else
#define TEGRA_GM20B_ASSERT(x, n) _Static_assert(sizeof(struct x) == (n), #x " layout")
#endif
TEGRA_GM20B_ASSERT(TegraGM20BAbiInfo, 192);
TEGRA_GM20B_ASSERT(TegraGM20BAbiBoAlloc, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiBoAllocReply, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiBoImport, 24);
TEGRA_GM20B_ASSERT(TegraGM20BAbiSurfaceWrites, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiSurfaceLayout, 32);
TEGRA_GM20B_ASSERT(TegraGM20BAbiSurfaceLayoutReply, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiSurfaceLayoutInfo, 48);
TEGRA_GM20B_ASSERT(TegraGM20BAbiBoFree, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiBindOp, 40);
TEGRA_GM20B_ASSERT(TegraGM20BAbiVmBind, 4008);
TEGRA_GM20B_ASSERT(TegraGM20BAbiChannelCreate, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiCpuComplete, 32);
TEGRA_GM20B_ASSERT(TegraGM20BAbiCpuRange, 24);
TEGRA_GM20B_ASSERT(TegraGM20BAbiCpuCompleteReply, 24);
TEGRA_GM20B_ASSERT(TegraGM20BAbiFenceIdentity, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiChannelCreateReply, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiChannel, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiPush, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiExec, 4088);
TEGRA_GM20B_ASSERT(TegraGM20BAbiExecReply, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiSurfaceUse, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiExecSurfaces, 1040);
TEGRA_GM20B_ASSERT(TegraGM20BAbiWaitFence, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiTimestamp, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiStatus, 24);
TEGRA_GM20B_ASSERT(TegraGM20BAbiMap, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiMapReply, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiUnmap, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiOrder, 16);
TEGRA_GM20B_ASSERT(TegraGM20BAbiOrderReply, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiPriority, 8);
TEGRA_GM20B_ASSERT(TegraGM20BAbiZbc, 40);
TEGRA_GM20B_ASSERT(TegraGM20BAbiZbcReply, 8);
#undef TEGRA_GM20B_ASSERT
#endif
