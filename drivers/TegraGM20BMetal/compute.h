// SPDX-License-Identifier: MIT
// The driver's compute work: Metal kernels as MAXWELL_COMPUTE_B launches on the raw channel, beside its 3D and copy
// work. The pack's context init binds the class on subchannel 1 with the raw heap as its program region and the raw texture
// and sampler pools (pack.h PackInit), so a dispatch is a QMD in the upload ring and SEND_PCAS: no compute state lives in
// the channel between dispatches.
//
// A kernel is NVK's work for one Metal kernel at one key (compute_key.h: m2v compiles the threadgroup shape and memory
// lengths into the program, and picks its variant by the textures and samplers bound): NAK's program, the QMD NVK filled for
// it (nak_fill_qmd), which of its constant buffers are NVK's root table, set 0 and shader data, and where set 0 takes each
// Metal buffer, texture and sampler (pack.h PackKernel). A dispatch writes those as NVK's nvk_cmd_upload_qmd does: its
// own root table (the cs group count, the set's address, the push words with m2v's DispatchRegion field) and set 0 in the
// upload ring, and a QMD of the template with the program's heap offset, the grid and the constant buffers' addresses.
//
// Order. A compute encoder is ordered after the work encoded before it and before the work after it on the GPU, as Metal orders
// a serial encoder: a wait for idle where it starts (the render and copy work before it has written), between its dispatches
// a shader-data flush that waits for the one before, and where it ends a wait for idle with the texture and constant caches
// invalidated (what the 3D and copy engines read next was written through L2).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "band_rates.h"
#include "compute_record.h"
#include "dispatch_bands.h"
#include "dispatch_regions.h"
#include "gpu.h"
#include "icb_compute.h"
#include "loop_copies.h"
#include "spill_table.h"
#include "stage_in.h"

namespace gm20b {

constexpr uint32_t kQmdWords = 64;

struct BufferDescriptor;

// Kernel buffer entry `e`'s descriptor (residual.h bufferDescriptor). An entry with no residual word is a slot
// its program never addresses: the translator places a bind offset for every slot the program reads, and a kernel
// whose plan changes under a bind offset, whose record would hold no word at all, is refused at its compile
// (direct_compute.cpp attachResiduals). It binds at its base rounded down to 16 bytes.
void kernelBufferDescriptor(const PackKernelEntry &e, uint64_t va, uint32_t size, BufferDescriptor &d);

// A kernel ready to dispatch: its record and where its program lies in the heap
struct ComputeState {
    const PackKernel *rec = nullptr;
    uint32_t instructions = 1;
    uint8_t cbufType[8] = {};
    uint32_t program = 0;  // QMD PROGRAM_OFFSET: from the program region (the heap's start)
    uint64_t data = 0;     // its shader data's address, 0: none
    PackKernel placed =
        {};  // a runtime record's, its samplers numbered among the device's (placeCompute): rec points here
    mutable BandRate rate;  // what its dispatches cost the GPU, learned from its measured ones (band_rates.h)
    // its reads through argument buffers (Device::bindKernelArguments): the root push words of the device's root table and
    // ExecutionStatus addresses, its set 1 (null: none), and whether the device had no memory for them
    uint32_t argumentPush[32] = {}, argumentMask = 0;
    const HeapSet *heapSet = nullptr;
    bool heapMissing = false;
    const uint32_t *spill = nullptr;  // its argument table's words (spill_table.h): the pack's or its record's
};

// What a refused compute command is to its command buffer: Metal's out-of-memory error where the device had no memory
// or header left for it, Metal's validation error for a request Metal stops, else the driver's own error (a record or
// plan breaking what its making guarantees, or a form the driver has not)
enum class ComputeFailure : uint8_t {
    Internal,
    OutOfMemory,
    Invalid,
};

// The pack's kernel of key `keyHash` (computeKeyHash), its program placed at its first use; null: not in the pack, or no
// room in the heap
const ComputeState *computeState(Device &dev, uint64_t keyHash);
// A record of no pack, its program placed at its first use (null: no room)
const ComputeState *placeCompute(Device &dev, const ComputeRecord &r);

class ComputeEncoder {
public:
    ComputeEncoder(CommandBuffer &cb, Device &dev);

    void setState(const ComputeState *k)
    {
        state_ = k;
    }

    // MTLDispatchTypeSerial: each dispatch after the encoder's first waits for the one before, its writes visible
    void setSerial(bool serial)
    {
        serial_ = serial;
    }

    void setBytes(uint32_t slot, const void *bytes, uint32_t length);
    void setBytesAt(uint32_t slot, uint64_t va, uint32_t length,
                    const Buffer *allocation = nullptr);  // bytes already where the GPU reads them
    // The kernel's own memory mapped for the connection (its clock anchor table): bound, never a job's use, since no BO of
    // the client's holds it and SubmitJob refuses a use it cannot find
    void setKernelBytes(uint32_t slot, uint64_t va, uint32_t length);
    void setBuffer(uint32_t slot, const Buffer *buffer, uint64_t offset);
    void setBufferOffset(uint32_t slot, uint64_t offset);
    // A stage-in kernel's: a buffer slot's bind stride (UINT32_MAX: none), which a layout of a dynamic stride reads, and
    // setStageInRegion:'s origin and size, both written into its push words at each dispatch
    void setBufferStride(uint32_t slot, uint32_t stride);
    void setStageInRegion(const std::array<uint32_t, 6> &region);
    void setTexture(uint32_t slot, const Texture *t);
    void setSampler(uint32_t slot, const Sampler *s);

    // dispatchThreadgroups: `groups` threadgroups of `threads` threads, the shape the kernel's record was made at. False:
    // refused (*why), nothing encoded
    bool dispatch(const uint32_t groups[3], const uint32_t threads[3], const char **why);
    // dispatchThreadgroupsWithIndirectBuffer: the grid is the three words at `offset` in `arguments` as the GPU reads
    // them when it reaches the dispatch (icb_compute.cpp), in threadgroups of `threads`. False: refused (*why),
    // nothing encoded
    bool dispatchIndirect(const Buffer *arguments, uint64_t offset, const std::array<uint32_t, 3> &threads,
                          const char **why);
    // setStageInRegionWithIndirectBuffer: the dispatches after it take their stage-in origin and size from the six words
    // at `offset` in `region` as the GPU reads them at each dispatch; null: setStageInRegion:'s words again
    void setStageInRegionIndirect(const Buffer *region, uint64_t offset);
    bool dispatchRegion(const ComputeRegion &region, const std::array<uint32_t, 3> &grid,
                        const std::array<uint32_t, 3> &nominal, bool first, bool last, const char **why);
    // memoryBarrier...: the dispatches before have written, and their writes are visible to the ones after
    void barrier();
    // A predication record (predication.h): the word at `wordVa` for encodeStartIf; Misused: nothing encoded
    Predication::Answer startIf(uint64_t wordVa, PredicateCompare compare, uint32_t reference);
    Predication::Answer startElse();
    Predication::Answer endIf();
    // Loops (predication.h, loop_copies.h): a While's word at `wordVa`, a DoWhile's at its end; `tag` names the
    // loop in the record its end leaves where it would run past its copies (Device::loopContinue). Misused: a loop the
    // raw path cannot lay down (deeper than LoopCopies::kMaxDepth, a body of more than kMaxSegments segments or one that
    // left the encoder, no copies within the entry budget), nothing more encoded
    Predication::Answer startWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference);
    Predication::Answer endWhile(uint32_t tag);
    Predication::Answer startDoWhile();
    Predication::Answer endDoWhile(uint64_t wordVa, PredicateCompare compare, uint32_t reference, uint32_t tag);
    // A counter sample on the compute class now: right after the encoder's opening wait for idle, its start
    void timestamp(uint64_t va);
    void end();

    // What the refusal that ended this encoder's work is to its command buffer (no encoder goes on past one); a command
    // encoded through it from outside sets its own with fails()
    ComputeFailure failure() const
    {
        return failure_;
    }

    void fails(ComputeFailure failure)
    {
        failure_ = failure;
    }
#if defined(SWITCH_METAL_HARNESS_MARKS)
    // The bench's QMD chains (hw_test_app/bench qmdchain): each launch's QMD as built, before its upload, to patch;
    // false holds the launch back, the QMD then only a copy another QMD names dependent
    std::function<bool(uint32_t *qmd)> benchQmd;
#endif

    // A buffer an indirect dispatch command binds at a slot (va 0: none)
    struct IcbBuffer {
        uint64_t va = 0;
        uint32_t size = 0;
    };

    // One region of an indirect dispatch command of kernel `k` (icb_compute.h): its QMD, root table and set 0 written at
    // `block` (GPU address `va`, icbRegionBytes(*k.rec) long), the command's stage-in words in its root table. False:
    // refused (*why), as a dispatch is.
    static bool icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region,
                          const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                          const IcbBuffer (&buffers)[31], const StageIn &stageIn, uint8_t *block, uint64_t va,
                          const char **why);

    // A region's block past icb::computeRegionBytes, each part 256-byte aligned: a kernel's root table of the dispatch's
    // buffer slots (PackKernel::rootSlots), as a dispatch's (argumentRoot), at `slots`; its argument table of bind offsets
    // (spill_table.h) at `spill`; `bytes` in all
    struct IcbRegionLayout {
        uint32_t slots = 0, spill = 0, bytes = 0;
    };

    static IcbRegionLayout icbRegionLayout(const PackKernel &r);
    static uint32_t icbRegionBytes(const PackKernel &r);
    // One launch of a region past the QMD's grid fields: its slice (dispatch_bands.h qmdSlice)
    static bool icbRegion(Device &dev, const ComputeState &k, const ComputeRegion &region, const DispatchPiece &slice,
                          const std::array<uint32_t, 3> &grid, const std::array<uint32_t, 3> &nominal,
                          const IcbBuffer (&buffers)[31], const StageIn &stageIn, uint8_t *block, uint64_t va,
                          const char **why);
    // The set-0 buffer descriptors, StageInStride root words and root table slot entries of a region of kernel `k`
    // written at `va` (icbRegion), as a buffer that inherits the encoder's buffers binds them at an execute
    // (icb_compute.h ComputeBind, ComputeStride, ComputeRoot). False: refused (*why).
    static bool icbBinds(const ComputeState &k, uint64_t va, std::vector<icb::ComputeBind> &out,
                         std::vector<icb::ComputeStride> &strides, std::vector<icb::ComputeRoot> &roots,
                         const char **why);
    // A kernel placed while the device had no memory for its argument tables binds them now, as a dispatch that meets it
    // does; false: still none
    static bool icbArgumentTables(Device &dev, const ComputeState &k);
    // A command of a buffer that inherits the encoder's buffers takes the ones this encoder binds now, each used as written,
    // and their bind strides
    void inheritBuffers(IcbBuffer (&out)[31], std::array<uint32_t, 31> &strides);

    struct IcbExecute {
        const Buffer *slots;  // the commands' words, slotWords each, command `first`'s at slot `at`
        uint32_t at, first, count;
        const Buffer *range;  // the execution range the GPU reads at rangeOffset; null: [first, first + count)
        uint64_t rangeOffset;
        bool written;  // a blit wrote the words on the GPU
        // a buffer that inherits the encoder's buffers: its bind slots (bindWords each, as `slots`), the Metal buffer
        // indices they read, and those whose bind stride a stage-in command reads
        const Buffer *binds = nullptr;
        uint32_t bindSlots = 0, strideSlots = 0;
        uint32_t slotWords = icb::kComputeSlotWords, bindWords = icb::kComputeBindSlotWords;
    };

    // The commands by reference, ordered after this encoder's work before them as a dispatch is
    bool executeSlots(const IcbExecute &x, const char **why);

private:
#ifdef SWITCH_METAL_HARNESS_MARKS
    friend struct DeviceHarness;
#endif
    void argumentRoot(const PackKernel &r, uint32_t *root);
    // The root words of kernel `k`'s reads through argument buffers that every dispatch of it takes alike: the device's
    // root table and ExecutionStatus addresses, its set 1 (Device::bindKernelArguments)
    static void argumentWords(const ComputeState &k, const PackKernel &r, uint32_t *root);
    // A handle table entry's set-0 descriptor: the device's texture or sampler table (heap_tables.h)
    static void tableDescriptor(const Device &dev, const PackKernelEntry &e, uint32_t *at);
    static void rootWords(const PackKernel &r, const uint32_t base[3], const uint32_t groups[3], uint64_t setVa,
                          uint32_t residualMask, const std::array<uint32_t, 32> &residuals, const uint32_t region[8],
                          uint32_t *root);
    static uint32_t activeRootBytes(const ComputeState &k, const PackKernel &r, uint32_t residualMask,
                                    bool indirectStageIn);
    static bool qmdWords(const ComputeState &k, const uint32_t groups[3], uint64_t rootVa, uint64_t setVa,
                         uint32_t *qmd, const char **why, uint32_t rootBytes = 0x800);
    // Why an indirect dispatch command cannot carry kernel `k`, null where it can
    static const char *icbRefusal(const ComputeState &k);
    // The header index sampler entry `e`, whose record holds its pairs, binds: its own, where the address maps of the
    // textures it samples change none of its modes (sampler_map.h) or where its record was made under the maps the
    // dispatch's key states (`mapsStated`, PackKernel::mapsStated); null, or why it has none
    static const char *pairedSamplerTsc(const Device &dev, const PackKernelEntry &e,
                                        const Texture *const (&textures)[31], const Sampler *const (&samplers)[16],
                                        bool mapsStated, uint32_t &tsc);

    struct Bound {
        uint64_t va = 0;
        uint32_t size = 0;
        const Buffer *buffer = nullptr;
        uint64_t offset = 0;
        uint64_t resourceVa = 0;
        bool kernelMemory = false;

        bool operator==(const Bound &b) const
        {
            return va == b.va && size == b.size && buffer == b.buffer && offset == b.offset &&
                   resourceVa == b.resourceVa && kernelMemory == b.kernelMemory;
        }
    };

    CommandBuffer &cb_;
    Device &dev_;
    Push p_;
    uint32_t *start_ = nullptr;
    const ComputeState *state_ = nullptr;
    Bound bufs_[31];
    const Texture *textures_[31] = {};
    const Sampler *samplers_[16] = {};
    bool serial_ = false, dispatched_ = false;
    uint64_t apiWork_ = 0;
    uint64_t bindingRevision_ = 0, describedRevision_ = 0;
    const ComputeState *describedState_ = nullptr;
    std::vector<uint32_t> descriptorWords_;
    std::array<uint32_t, 32> residuals_{};
    uint32_t residualMask_ = 0;
    SpillResiduals spilled_;
    uint64_t setVa_ = 0, rootVa_ = 0;
    uint32_t rootBytes_ = 0x800;
    uint64_t slotTableVa_ = 0;  // this dispatch's argument root-slot table, shared by its band pieces (argumentRoot)
    std::array<uint32_t, 3> grid_{}, base_{};
    std::array<uint32_t, 8> region_{};
    StageIn stageIn_;
    const Buffer *stageInRegion_ = nullptr;
    uint64_t stageInRegionOffset_ = 0;

    struct AccessRange {
        uint64_t lo = 0, hi = 0;
    };

    static constexpr uint32_t kHazardRanges = 32;
    AccessRange curReads_[kHazardRanges] = {}, curWrites_[kHazardRanges] = {};
    uint32_t curReadCount_ = 0, curWriteCount_ = 0;
    bool curUnknownRead_ = false, curUnknownWrite_ = false;
    AccessRange pendingReads_[kHazardRanges] = {}, pendingWrites_[kHazardRanges] = {};
    uint32_t pendingReadCount_ = 0, pendingWriteCount_ = 0;
    bool pendingUnknownRead_ = false, pendingUnknownWrite_ = false;
    bool dispatchHasHazard(const AccessRange *extraReads = nullptr, uint32_t extraReadCount = 0) const;
    void recordDispatchAccesses(const AccessRange *extraReads = nullptr, uint32_t extraReadCount = 0);
    // `bytes` at `dst` written by the compute class's inline engine from the same bytes at `src`, which the channel
    // fetches after the method header as a continuation of the stream (nvk_cmd_compute_indirect_copy)
    void inlineFrom(uint64_t dst, uint64_t src, uint32_t bytes);
    // The stage-in region words of the root table at `rootVa` from setStageInRegionWithIndirectBuffer:'s buffer;
    // `fetched`: the stream already waits for the writes before
    void stageInFromMemory(const PackKernel &r, uint64_t rootVa, bool fetched = false);
    void bindBuffer(uint32_t slot, const Bound &binding);
    bool argumentTables(const char **why);
    bool prepareDescriptors(const char **why);
    void room(uint32_t words);
    void emitKindSwitch();
    bool launch(const std::array<uint32_t, 3> &grid, const DispatchPiece &piece, const std::array<uint32_t, 8> &region,
                const char **why);
    void cutBatch();
    void noteDispatch(uint64_t units, bool looping, uint32_t pieces);
    uint32_t dispatches_ = 0;
    ComputeFailure failure_ = ComputeFailure::Internal;
    DrawTiming candidate_;
    LoopStream stream();
    Predication::Answer startLoop(bool doWhile, uint64_t wordVa, const PredicateTest *test);
    Predication::Answer endLoop(uint64_t wordVa, const PredicateTest *test, uint32_t tag);
};

}  // namespace gm20b
