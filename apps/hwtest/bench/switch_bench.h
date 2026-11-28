// HW Test's bench loop on the GPU (tools/benchloop): caller-supplied Maxwell programs and MAXWELL_B methods drawn on a raw
// device of its own (gpu.h over its own connection and GPU VM), each command buffer timed by its PTIMER
// stamps. Nothing here changes the app's Metal device.
#ifndef SWITCH_BENCH_H
#define SWITCH_BENCH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum switch_bench_kind {
    SWITCH_BENCH_NOOP,      // empty passes: the fixed cost of a command buffer
    SWITCH_BENCH_KNOWN,     // the app's own constant-colour program (bench_known.h): proves program injection
    SWITCH_BENCH_FRAGMENT,  // the caller's pixel program on a quad
    SWITCH_BENCH_COMPUTE,   // the caller's kernel
    SWITCH_BENCH_READBACK,  // the caller's pixel program with the caller's methods around its draw
    SWITCH_BENCH_TWOD,      // one 2D-engine fill, scaled copy or resolve into the target (probe_ops.h), no program
    SWITCH_BENCH_PACK,  // a pack state with its own programs, the caller's uniform blocks a quad (switch_bench_pack)
};

// How a pack experiment's draws take a state with a split (split_reuse.h SplitProbe). Every form but whole takes
// the split plan's draws without a plan, so a bench-sized quad splits as a screen-sized one would.
enum switch_bench_pack_form {
    SWITCH_BENCH_PACK_WHOLE,          // whole programs, the fact-selected variant (forceWholePrograms)
    SWITCH_BENCH_PACK_WHOLE_GENERIC,  // the generic whole program the split twin is made from, no prologue
    SWITCH_BENCH_PACK_SPLIT,          // the split twin, its frontier from its prologue
    SWITCH_BENCH_PACK_SPLIT_WHOLE,    // the prologue launched and its frontier written, the generic whole program drawn
};

// A uniform block: `bytes` at `offset` in the quad's own region of one buffer (an offset off 256 bytes binds its
// floor with the rest as a residual push word, as Core Animation's blocks do), bound at `slot` of `stage` (1 vertex,
// 2 fragment)
struct switch_bench_block {
    uint32_t stage, slot, offset, bytes;
    const uint8_t *data;
};

struct switch_bench_quad {
    uint32_t rect[4];  // x0, y0, x1, y1 in pixels
    const struct switch_bench_block *blocks;
    uint32_t block_count;
    // the draw's own vertex buffer bytes (a split capture's), drawn as Metal primitive `primitive` from vertex `first`,
    // `count` of them; null: the template's quad over rect
    const uint8_t *vertices;
    uint32_t vertex_bytes, primitive, first, count;
};

// Every quad drawn in one command buffer, in order, with the template's inputs (fetchform's) where no block is given
struct switch_bench_pack {
    uint32_t form;          // switch_bench_pack_form
    uint32_t ca_transform;  // the state's vertex function is Core Animation's fixed_vert_l?h_spc
    const struct switch_bench_quad *quads;
    uint32_t quad_count;
};

enum switch_bench_twod_op {
    SWITCH_BENCH_TWOD_FILL,
    SWITCH_BENCH_TWOD_SCALE,    // from the source
    SWITCH_BENCH_TWOD_RESOLVE,  // the source's corner copied 1:1 into a multisampled image's sample grid, resolved
};

// The same operation again through another unit, into a second target with the same fill
enum switch_bench_compare {
    SWITCH_BENCH_COMPARE_NONE,
    SWITCH_BENCH_COMPARE_PROGRAM,    // twod fill or scale: the caller's pixel program drawn over the rect
    SWITCH_BENCH_COMPARE_COPY,       // twod 1:1 point scale: the copy engine copying the source
    SWITCH_BENCH_COMPARE_RESOLVE3D,  // twod resolve: the pack's resolve state (samples.h), as Metal's resolve draws
    SWITCH_BENCH_COMPARE_PLAIN,      // a compressed readback: the same draws into a plain target
    SWITCH_BENCH_COMPARE_SAMPLED3D,  // twod bilinear scale: the pack's mip state (mip.h) sampling the source
};

struct switch_bench_twod {
    uint32_t op;
    uint32_t fmt2d;           // the destination's 2D surface format; 0: the target format's (gm20b::probeTwodFormat)
    uint32_t color_format;    // SET_RENDER_SOLID_PRIM_COLOR_FORMAT
    uint32_t color[4];        // one packed word, or four floats' bits for RF32_GF32_BF32_AF32
    uint64_t du, dv, sx, sy;  // 32.32 source step per destination texel and source origin
    uint32_t bilinear, corner;
    uint32_t reference;    // the reference fill: SRCCOPY, no colour key, clip at the surface (ProbeOp::referenceFill)
    uint32_t bracketed;    // between the production 2D path's waits (ProbeOp::bracketed)
    uint32_t samples;      // a resolve's: the multisampled image's samples a pixel
    uint32_t clip[4];      // the 2D engine's clip rectangle x0, y0, x1, y1 (ProbeOp::clip); x1 0: none
    uint32_t clear_first;  // a clear-only pass on the target just before the operation, in its command buffer
    uint32_t tiled;        // a block-linear target with no CPU view, its texels in and out through the copy engine
    // a resolve read by a scale of the sample grid with this operation's du, dv, sx, sy, bilinear and corner, in place
    // of the driver's resolve (probe_ops.h probeResolveOp)
    uint32_t resolve_by_scale;
    uint32_t rop;  // fill, scale, a by-scale resolve: a ROP3 code on the destination's codes (ProbeOp::rop); 0 SRCCOPY
    // a by-scale resolve: the sample grid complemented in place (a fill under ROP3 DSTINVERT) before the scale reads it
    uint32_t invert_grid;
};

// What a scale, resolve or copy reads: rows packed (null: the bench's own 64x64 BGRA8 picture)
struct switch_bench_source {
    uint32_t format, width, height;
    const uint8_t *data;
};

enum switch_bench_status {
    SWITCH_BENCH_OK,
    SWITCH_BENCH_INVALID,  // the description is not one the driver can take
    SWITCH_BENCH_REFUSED,  // the driver refused the state, the pass or a draw
    SWITCH_BENCH_KILLED,   // the kernel killed the channel while it ran
    SWITCH_BENCH_ERROR,
};

struct switch_bench_program {
    const uint8_t *sph;  // 80 bytes; null for a kernel
    const uint8_t *code;
    size_t code_bytes;
    const uint8_t *data;  // bound at constant buffer 1
    size_t data_bytes;
    uint32_t gprs;
    uint32_t data_bound;  // bytes constant buffer 1 is bound with; 0: the data padded to 256-byte steps
};

struct switch_bench_experiment {
    uint32_t kind;
    uint32_t format, width, height;  // MTLPixelFormat
    uint32_t rect[4];                // x0, y0, x1, y1 in pixels
    float clear[4];
    const uint8_t *fill;  // the target's texels before each command buffer, rows packed (null: cleared)
    struct switch_bench_program program;
    uint32_t threads[3], groups[3];
    const uint32_t *draws;  // draws (dispatches) a command buffer, one entry per configuration
    uint32_t draw_counts;
    uint32_t repeats, warmup;
    const uint32_t *methods;  // (method, value) pairs before the draw, then restore after it
    uint32_t method_pairs;
    const uint32_t *restore;
    uint32_t restore_pairs;
    uint32_t single_target;  // readback: colour 0 bound alone, the template's other colours left out
    uint64_t template_key;  // the pack state to draw with (bench_records.h fragmentTemplate); 0: the bench's own choice
    struct switch_bench_twod twod;
    struct switch_bench_source source;
    uint32_t compare;       // switch_bench_compare
    uint32_t compressed;    // readback: colour 0 in compressible memory, its texels in and out through the copy engine
    uint32_t output_bytes;  // compute: a buffer of this size, its address the shader data's first 8 bytes
    // compute: the QMDs a chain names dependent after the experiment's own (QMD 0), each kernel's shader data bytes 8-15
    // the address of QMD 1 (QMD i at 256 * (i - 1) past it). Only QMD 0 is launched; bit i of chain_schedule: QMD i
    // schedules QMD i + 1 when it completes. Every QMD's release adds 1 to the output's last word.
    const struct switch_bench_program *chain;
    uint32_t chain_links, chain_schedule;
    struct switch_bench_pack pack;
    // fragment, readback: a zeroed GPU buffer of storage_bytes (0: none) for the experiment's life, its 64-bit address
    // written into the shader data at byte storage_va_at
    uint32_t storage_bytes, storage_va_at;
};

// What the app did, so a result proves its own form
struct switch_bench_form {
    uint32_t set;
    uint32_t comptags, block_linear;  // the written target's
    uint32_t bracketed, reference, samples;
    uint32_t clipped, clear_first;
    uint32_t rop, invert_grid;
};

struct switch_bench_result {
    uint32_t status;
    char why[256];
    uint64_t *gpu_ns;   // draw_counts x repeats, the caller's; 0 where the stamps were overwritten
    uint64_t *wall_ns;  // commit to the fence, mach_absolute_time
    uint8_t *texels;    // width x height texels, rows packed, the caller's (null: none read)
    size_t texel_bytes;
    uint32_t lost_error;
    uint64_t fault_va;
    uint64_t template_key;  // the pack state whose vertex program and registers the draw took
    uint8_t *output;        // the kernel's output buffer after the last dispatch, the caller's (null: none)
    size_t output_bytes;
    uint8_t *compare_texels;  // the compare form's target, as texels (null: none)
    uint32_t compare_status;  // the compare form's own status and why: its failure leaves the experiment's result
    char compare_why[160];
    struct switch_bench_form form;
    // pack: the last command buffer's prologue launches and their frontiers' bytes, one after another in launch order
    // (frontier: the caller's, frontier_bytes its room in and what was written out)
    uint32_t launches;
    uint8_t *frontier;
    size_t frontier_bytes;
    uint8_t *storage;  // the storage buffer after the last command buffer, the caller's (null: none read)
    size_t storage_bytes;
};

struct switch_bench;

// The bench's raw device over the installed pack; null with *why
struct switch_bench *switch_bench_open(char *why, size_t why_size);
void switch_bench_close(struct switch_bench *bench);
// Bytes per texel of a target format the driver renders, 0 for any other
uint32_t switch_bench_texel_bytes(uint32_t format);
// One line: the pack, the connection and the templates found
size_t switch_bench_describe(struct switch_bench *bench, char *out, size_t size);
// The known-answer program's shader data, which its texels must equal (bench_known.h)
void switch_bench_known_data(uint32_t out[4]);
// A kernel of nothing but EXIT, for the compute self-check; its byte count (0: `size` too small)
size_t switch_bench_known_kernel(uint8_t *out, size_t size);
// Runs on the caller's thread; one experiment at a time
void switch_bench_run(struct switch_bench *bench, const struct switch_bench_experiment *e,
                      struct switch_bench_result *r);
// One 2D-engine fill of all of `surface` (an IOSurfaceRef of one plane, `format` a MTLPixelFormat the driver renders)
// with `color`, one A8B8G8R8 word, on the bench's own connection and channel, waited for: a status, and *why
void switch_bench_fill_surface(struct switch_bench *bench, const void *surface, uint32_t format, uint32_t color,
                               struct switch_bench_result *r);

// gpu.kernel.sparse_map's readings (bench_sparse.cpp): 1 where a step held, 0 where it did not, -1 where an earlier
// failure stopped the run (`why` says which)
struct switch_bench_sparse {
    int32_t info_bit;
    int32_t head_bytes_kept, tail_read_zero, big_read_zero, beside_big_read_zero, write_dropped;
    int32_t replaces_map, map_replaces, bad_args_refused, faults, unmapped_read_faults;
    int32_t bad_args_kr[2];          // the two VmBinds' kern_return_t
    uint32_t fault_error;            // the throwaway channel's TegraGM20BAbiStatus.error after its read
    uint64_t fault_va, unmapped_va;  // its fault VA, and the VA it read
    char why[256];
};

// Every step on a TegraGM20B connection of its own (no Metal device), on the caller's thread; the last kills a
// throwaway channel of that connection with an MMU fault
void switch_bench_sparse_map(struct switch_bench_sparse *r);

// gpu.kernel.channel_epoch's readings (bench_channel_epoch.cpp): -1 where an earlier failure stopped the run (`why`
// says which); epoch_* are the fence page's TegraGM20BChannelEpochWord: before the refused ask, after it, right after
// a channel was destroyed (epoch_after) and in the other connection's page then (epoch_other)
struct switch_bench_channel_epoch {
    int32_t epoch_flag, slots_made, refused_kr, recreate_kr, job_kr, wait_kr, job_filled;
    uint32_t fence_token, epoch_before, epoch_refused, epoch_after, epoch_other;
    uint64_t destroy_to_create_ns;
    char why[128];
};

// Every step on two TegraGM20B connections of its own (no Metal device), on the caller's thread; nothing is left open
void switch_bench_channel_epoch_run(struct switch_bench_channel_epoch *r);
// GetInfo's flags (TegraGM20BInfo*) on a connection of its own; 0 where no connection opened
uint32_t switch_bench_kernel_flags(void);
// GetInfo's flags and zcull[12], and method 'ZC''s reply (zcull's layout), on a connection of its own: ZC's
// kern_return_t, -1 where no connection opened
int32_t switch_bench_zcull_info(uint32_t *flags, uint32_t info[12], uint32_t zc[12]);
// GetInfo's GPC and TPC counts on a connection of its own: 0, -1 where no connection opened
int32_t switch_bench_gpu_units(uint32_t *gpcs, uint32_t *tpcs);
// The bytes of the bench device's interlock ticket buffer (the driver's Device::init allocation); 0 without a device
uint64_t switch_bench_ticket_bytes(struct switch_bench *bench);

// What follows the pattern pass of gpu.x1.l2_flush_writeback (bench_l2_flush.cpp), in a second pass of the same
// command buffer: host methods ahead of its one draw, then the job's end
enum switch_bench_l2_end {
    SWITCH_BENCH_L2_NONE,      // no methods; the job ends Internal, which flushes nothing (the control)
    SWITCH_BENCH_L2_BRACKET,   // WFI, FB_FLUSH, FB_FLUSH: the flush arms' bracket with no MEM_OP in it
    SWITCH_BENCH_L2_MEM_OP_D,  // the bracket around MEM_OP_C 0, MEM_OP_D L2_FLUSH_DIRTY (the kernel's form)
    SWITCH_BENCH_L2_MEM_OP_B,  // the bracket around MEM_OP_B L2_FLUSH_DIRTY alone (deko3d's form)
    SWITCH_BENCH_L2_JOB_END,   // no methods; the job ends External, the kernel's flush (gpu_exec.c chan_fence_job)
};

// One round: the CPU fills a write-combined pitch target with `sentinel` under a lease, the 3D engine draws `pattern`
// over all of it through the GPU's L2, the end runs, and once the fence passes the CPU reads the target through its
// write-combined mapping with no lease, so nothing but the end itself can have written the L2 back
struct switch_bench_l2_round {
    uint32_t end;                      // switch_bench_l2_end
    uint32_t pattern[4], sentinel[4];  // RGBA32Float texels, as bits
    uint32_t width, height;
    uint32_t *words;  // the caller's: width x height texels of 4 words, rows packed
    size_t word_count;
    uint32_t status;  // switch_bench_status
    char why[160];
    uint32_t exact_jobs;    // the kernel takes addressed jobs, so an Internal end can skip the flush
    uint32_t wc;            // the target's CPU view is the write-combined mapping (never CPU-cached)
    uint32_t block_linear;  // the target is block-linear (the round needs pitch rows)
    uint32_t emitted;       // draws that carried the end's methods
    uint32_t lost_error;
    uint64_t fault_va;
};

void switch_bench_l2_round_run(struct switch_bench *bench, struct switch_bench_l2_round *r);

#ifdef __cplusplus
}
#endif
#endif
