// SPDX-License-Identifier: MIT
// The raw GM20B path's pipeline pack (pack/mkpack.py writes it, pack/rawpack.py runs the whole generation at bundle build time;
// this is its layout). Little endian, packed, sections 256-byte aligned. Read-only after load: mmapped shared and pointed into,
// so every process reads the same page-cache pages. Its pipelines are in keyHash order (Pack::find searches them in place).
#pragma once
#include <cstddef>
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace raw {

#pragma pack(push, 1)

struct PackSection {
    uint64_t offset, bytes;
};

enum PackSectionId {
    PackPrograms,
    PackPipelines,
    PackSamplers,
    PackBaseline,
    PackInit,
    PackBlob,
    PackStrings,
    PackHeap,
    PackKernels,
    PackSplits,
    PackVariants,
    PackArchives,
    PackIterated,
    PackComputeStates,
    PackSectionCount
};

constexpr uint32_t kPackVersion = 23;

// What a record or a program variant (PackVariant) assumes, as bits of a mask: a draw takes a variant only where all of them
// hold. Bits 0-15 are facts of the draw's bindings, bit 16 of its pipeline, bit 26 of its pass, bits 17-25 of the chip
// (kChipAssumptions): 17 and
// 18 of the code a variant runs, 19-25 of the ROP blend a storage form computes in its program (PackPipeline::storageBlendModel
// holds them shifted down by kRopBlendShift, m2v rop_blend::Assumption's bits). A record is drawn whatever its ROP bits: they
// name what a device probe has to confirm before the program's blend can be called the ROP's.
enum PackAssumption : uint32_t {
    AssumeUniformAligned = 1u << 0,  // bits 0-7: bit k, PackPipeline::uniform[k]'s bind residual is 0
    AssumeTextureUnorm = 1u << 8,  // bits 8-15: bit k, the k-th EntryTexture entry's texel format is not a float format
    AssumePositionW1 = 1u << 16,   // the vertex stage writes position.w = 1
    // The chip's: the ROP converts an exact half to each target format's code as Metal does (an output left unrounded)
    AssumeRopRounding = 1u << 17,
    // The chip's: TEXS/TLDS/TLD4S.F16 give round-to-nearest-even of the f32 result their F32 forms give
    AssumeTexsF16 = 1u << 18,
    AssumeRopF32Terms = 1u << 19,        // blend terms and their sum in f32, each rounded to nearest even, unfused
    AssumeRopZeroOneFactors = 1u << 20,  // a factor of zero drops its term (an infinity or NaN too), one passes it
    // a normalized source clamped to [0, 1] (NaN 0), the store rounding to the nearest code
    AssumeRopUnormStore = 1u << 21,
    AssumeRopHalfStore = 1u << 22,     // a half store rounding to nearest even
    AssumeRopSrgbTables = 1u << 23,    // sRGB decoded and encoded by Metal's tables
    AssumeRopMinMaxNumber = 1u << 24,  // Min and Max answer the operand that is not a NaN
    AssumeRopDenormals = 1u << 25,     // f32 denormals flushed to zero
    // The pass draws into a turned plane (raw_rotation.h): the fragment program reads the frame through the root table's turn
    // words. Such a variant is its stage's first, so a turned draw takes it before any other.
    AssumeTurned = 1u << 26,
};

constexpr uint32_t kUniformRanges = 8, kTextureFacts = 8;
constexpr uint32_t kRopBlendShift = 19, kRopBlendAssumptions = 0x7Fu << kRopBlendShift;
constexpr uint32_t kChipAssumptions = AssumeRopRounding | AssumeTexsF16 | kRopBlendAssumptions;

// The PackAssumption bits of a record's storageBlendModel
constexpr uint32_t ropBlendAssumptions(uint8_t model)
{
    return uint32_t(model) << kRopBlendShift & kRopBlendAssumptions;
}

struct PackHeader {
    char magic[8];                                    // "GM20BPK1"
    uint32_t version, programs, pipelines, samplers;  // kPackVersion
    PackSection section[PackSectionCount];
    uint64_t stack;  // the build's stack key (build-tegra-gm20b-metal.py: the plugin is compiled with the same one)
    // the chip facts (kChipAssumptions) the pack's builder took as proven on the device: a draw's facts always hold them
    uint32_t chipFacts;
    uint32_t reserved;
};

// A program's constant buffer as the pack and the runtime's capture name it: its nvk_cbuf_type in bits 0-2, and for a uniform
// buffer's descriptor (CbufUbo) its byte offset in set 0 / 16 in bits 3-7. A uniform buffer of another set, or past 496 bytes,
// is CbufUnbindable: no raw draw binds it.
enum PackCbufType : uint8_t {
    CbufRoot = 1,
    CbufShaderData = 2,
    CbufSet = 3,
    CbufSplitParams = 4,  // a split prologue's parameters (PackSplit): the draw's push words and its frontier's address
    CbufUbo = 5,
    CbufFrontier = 6,  // a split fragment program's frontier (PackSplit): the draw's slice of its prologue's writes
    CbufUnbindable = 7
};

struct PackProgram {
    uint32_t heapOffset;  // slot start in the heap image
    uint32_t program;     // SET_PIPELINE_PROGRAM value (header at heapOffset + 0x30, code at + 0x80)
    uint32_t bytes;       // slot size
    // NAK's shader data in the slot (where NVK uploads it: 256-aligned after the code) and its size, a multiple of 256; 0: none
    uint32_t dataOffset, dataBytes;
    uint8_t cbufs;  // constant buffer slots it uses
    // each slot's PackCbufType, the root table at 0: as many as a GM20B stage binds (BIND_GROUP_CONSTANT_BUFFER's 18)
    uint8_t cbufType[18];
    uint8_t gprs;  // SET_PIPELINE_REGISTER_COUNT: a graphics program's registers (a kernel's are its QMD's)
    // SET_BINDLESS_TEXTURE's constant buffer slot + 1: where its bound texture instructions read their handles (word
    // index x 4, the handles' set-0 entries: PackPipeline::boundEntries); 0: it has none
    uint8_t boundTextures;
    uint8_t reserved[3];
};

// One of a stage's programs compiled under assumptions about its draws (PackAssumption). A stage's variants are tried in order
// and the first whose assumptions all hold is drawn; its generic program (PackPipeline::vs, fs) is the last, assuming nothing.
// A variant differs from the generic in code and register count only: the same constant buffers, early fragment tests,
// interlock and sample shading.
struct PackVariant {
    uint32_t assume;
    uint32_t program;
};

// A binary archive the IPSW's frameworks ship that declares states for the identity's GPU (pack/archives.py), so a creation
// under MTLPipelineOptionFailOnBinaryArchiveMiss misses where Metal's does. The PackArchives section: u32 archive count, u32 key
// count, the archives in pathHash order, then the keys, each archive's ascending: Pack::fnv of a render state's key
// (RawPipelineKey.h) or of a compute state's archive key (raw_compute_key.h computeArchiveKey).
struct PackArchive {
    uint64_t pathHash;  // Pack::fnv of the archive file's path on the device (rmArchivePath)
    uint32_t first, count;
};

// The PackArchive whose keys are the compute states the pack holds kernels for, at any dispatch shape (computeStateKey)
constexpr uint64_t kPackHeldKernels = 0;

// A compute state's answers to Metal's per-state queries as m2v's front end gave them when the pack was made (its
// ComputePipelineMetalInfo, which no kernel's compile changes), so a state of the pack is made without m2v. The
// PackComputeStates section, ascending by keyHash: Pack::fnv of its computeStateKey.
struct PackComputeFacts {
    uint64_t keyHash;
    uint32_t maxThreads;      // maxTotalThreadsPerThreadgroup
    uint32_t executionWidth;  // threadExecutionWidth
    uint32_t staticMemory;    // staticThreadgroupMemoryLength
    uint32_t reserved;
};

// The rung of the fetch ladder a color attachment's framebuffer fetch needs: FetchNone where it is not fetched
enum PackFetchRung : uint8_t {
    FetchNone,
    FetchBarrier,        // a texture read behind a pixel barrier: the state does not write the color
    FetchBlend,          // fixed-function blend state, the program fetching nothing
    FetchIteratedBlend,  // the ROP's iterated blend
    FetchInterlock       // a storage image inside the pixel interlock, any blend computed in the program
};

// PackPipeline::fetchRung's low bits are its PackFetchRung; its high bits, the rungs below FetchInterlock a color's fetch could
// take: Core Animation's fragment blend of the color has a fixed-function equation (FetchBlend), or has none
// (FetchIteratedBlend). Where the pack holds the state's fetch-free form (PipelineFetchFreeForm), that record's fetchRung
// names the rung it draws on for the color it blends.
constexpr uint8_t kFetchRungMask = 0x0F, kFetchCandidateBlend = 0x10, kFetchCandidateIterated = 0x20;

// A fetch-free form's iterated blend (PackIterated, one per program the pack's records name): the program the ROP runs for
// the one colour target its draw binds, and the common blend block's alpha factors it takes alpha from (enable 1). A
// program of no instructions is Core Animation's colour matrix (vcm), whose constants and form are each draw's uniform:
// the GPU's macro SWITCH_GM20B_MACRO_VCM_CONSTANTS loads them from the fragment buffer and offset it names.
constexpr uint32_t kIteratedProgramWords = 128, kIteratedConstants = 8;

struct PackIteratedBlend {
    uint8_t count;   // instructions, SET_ITERATED_BLEND_PASS; 0: the colour matrix macro's
    uint8_t enable;  // SET_ITERATED_BLEND: 1 RGB from the program, alpha from the common block; 3 both from the program
    uint8_t constants;       // constant slots it reads, from 0
    uint8_t uniformSlot;     // the colour matrix's: the fragment buffer index it is in; kNoUniform: none
    uint16_t uniformOffset;  // the matrix's bytes from the buffer's bind offset
    uint8_t uniformWords;    // its words there: five half4 rows, then the clamp pair where its QuartzCore has one
    uint8_t reserved;
    uint32_t alphaSource, alphaDest;  // SET_BLEND_ALPHA_SOURCE_COEFF, SET_BLEND_ALPHA_DEST_COEFF (OGL values)
    uint64_t name;  // FNV-1a 64 of the program's name (pack/iterated/table.py; vcm), which raw_chip_facts.h proves
    uint64_t reservedName;
    uint16_t constant[kIteratedConstants][3];  // SET_ITERATED_BLEND_CONSTANT_RED/GREEN/BLUE(j): half floats
    uint32_t program[kIteratedProgramWords];   // LOAD_ITERATED_BLEND_INSTRUCTION words, the first count of them
};

// The bytes of a uniform buffer entry a pipeline's programs read: from its bind offset, in 16-byte units
struct PackUniformRange {
    uint8_t entry;       // PackPipeline::entry index; kNoUniform: none
    uint8_t bank;        // the constant buffer slot the reading stage's programs bind it at
    uint8_t first, end;  // [first, end); end 0: unbounded (a read at an offset the program computes)
};

constexpr uint8_t kNoUniform = 0xFF;

enum PackEntryKind : uint8_t {
    EntrySamplerOnly = 0,
    EntryUbo = 1,
    EntryTexture = 2,
    EntryFetch = 3,
    // A texture3d argument (Core Animation's colour cubes) at fragment texture index `slot`; a texture of another type bound
    // there leaves its handle empty (RenderEncoder::prepare). The warm-up probe's record has none (kNoSlot).
    EntryTexture3d = 4,
    // The colour table m2v's program reads to write an 8-bit sRGB colour target as Metal does (raw_srgb_table.h): a storage
    // buffer descriptor of the device's own copy
    EntryColorTable = 5,
    EntryDepthTest = 6,
    EntryStencilTest = 7,
    // m2v's handle tables of argument-buffer textures and samplers (bind/heap.rs): a storage buffer descriptor of the
    // device's own table, every texture's by its header index, every sampler's by its sampler header's (raw_heap_tables.h)
    EntryTextureTable = 8,
    EntrySamplerTable = 9,
    // m2v's ArgumentTable of stage `stage` (raw_spill_table.h): a uniform buffer descriptor of the words the encoder uploads
    EntrySpillTable = 10,
    // A tessellation state's control-stage storage buffer (PackTessBuffer at `slot`), its residual word the control stage's
    EntryTessBuffer = 11
};

// What an EntryTessBuffer binds: the encoder's factor buffer, the draw's patch index buffer, its control-point index buffer
enum PackTessBuffer : uint8_t {
    TessFactors,
    TessPatchIndices,
    TessControlPointIndices,
    TessBufferCount
};

// The control stage's push words (m2v control::PUSH_FIELDS) from its first: each buffer's residual, then the factors'
// instance stride, the scale and whether a patch index buffer is bound
constexpr uint8_t kTessResidualWord[TessBufferCount] = {0, 3, 5};
constexpr uint8_t kTessInstanceStrideWord = 1, kTessScaleWord = 2, kTessIndexedWord = 4;

constexpr uint8_t kNoSlot = 0xFF;

enum PackSamplerKind : uint8_t {
    SamplerNone = 0,
    SamplerSlot = 1,
    SamplerConstexpr = 2
};

struct PackEntry {
    uint16_t offset;      // byte in the 256-byte set 0
    uint8_t kind;         // PackEntryKind
    uint8_t stage;        // 1 vertex, 2 fragment: whose buffer, texture and sampler slots the entry names
    uint8_t slot;         // UBO: Metal buffer index; texture: the stage's texture index; fetch: color attachment
    uint8_t samplerKind;  // handles: PackSamplerKind. Buffers: bufferStorage, bufferResidualWord
    uint8_t sampler;      // handles: the stage's sampler index, or constexpr sampler number. Buffers: the function's
    uint8_t pad;          //   window on the slot in 16-byte units (sampler | pad << 8), 0 = the whole buffer
};

// The texture and sampler slots of a render stage the raw encoder keeps (Metal's argument table on the A10X); a state
// whose function reads a texture past them is not packed
constexpr uint32_t kStageTextures = 31, kStageSamplers = 16;

// A handle entry's stage's index in the encoder's per-stage tables: vertex 0, fragment 1
inline uint32_t handleStage(const PackEntry &e)
{
    return e.stage == 1 ? 0 : 1;
}

// PackEntry::pad of a handle whose sampler slot the state samples by comparison: the sampler's compare header goes there
constexpr uint8_t kEntryCompares = 1;

inline bool entryCompares(const PackEntry &e)
{
    return e.kind != EntryUbo && e.samplerKind == SamplerSlot && (e.pad & kEntryCompares);
}

// A buffer entry: storage, else uniform (NVK rounds its range up to the 256-byte cbuf alignment)
inline bool bufferStorage(const PackEntry &e)
{
    return e.samplerKind & 1;
}

// An entry a program may read as a constant buffer (PackCbufType CbufUbo): a uniform buffer, or an argument table
inline bool uniformEntry(const PackEntry &e)
{
    return (e.kind == EntryUbo && !bufferStorage(e)) || e.kind == EntrySpillTable;
}

// A buffer entry's root push word (0x700 + 4 x word) that holds m2v's SlotBindOffset residual for its slot, -1 where the record
// names none (its states then take only offsets on m2v's base alignment: raw_residual.h)
inline int bufferResidualWord(const PackEntry &e)
{
    return int(e.samplerKind >> 2) - 1;
}

enum PackPipelineFlags : uint32_t {
    PipelineFetches = 1,
    // metal2vulkan's barrier form of a state whose first record fetches in the storage form (the pixel interlock): its
    // fetches read input attachments, ordered by barriers (pack/rawpack.py's probe with NVK's interlock hidden)
    PipelineBarrierForm = 2,
    PipelinePushMetadata = 4,
    PipelineStorageMetadata = 8,
    // one of the state's records for a variant its draws pick (the depth and stencil tests' class, the cubic samplers'
    // binds), made by the pack builder: under the state's keyHash, named by variantName; Pack::find passes over it
    PipelineVariantRecord = 16,
    // the state drawn with no framebuffer fetch: its program writes the blend's source, which the ROP blends with the
    // color (fetchRung: FetchBlend, its registers' blend; FetchIteratedBlend, the iterated program it names). Under the
    // state's keyHash; only PackForm::FetchFree finds it
    PipelineFetchFreeForm = 32,
    // its fragment program reads a turned plane's axes and no turned variant (AssumeTurned) could be made of it: a turned pass
    // does not draw it
    PipelineTurnRefused = 64,
    // its RGB10A8_2P_XR10 colours drawn in Metal's two planes (raw_xr_pass.h): the record of the state's " | xr2p" variant
    PipelineTwoPlane = 128
};

// Where a key's variant name starts (" | cubic ...", then " | tests=<n>", then " | xr2p", then " | xrs=<slots>", then
// " | wtex=<slot>:<format>,...": the runtime's names, RawMetalCubic.mm), its size where it names none
inline size_t variantNameAt(const std::string &key)
{
    return std::min({key.find(" | cubic "), key.find(" | tests="), key.find(" | xr2p"), key.find(" | xrs="),
                     key.find(" | wtex="), key.size()});
}

// Which of a state's records: its first (the storage form where it has both), its barrier form, or its fetch-free form
enum class PackForm : uint8_t {
    Default,
    Barrier,
    FetchFree
};

struct PackPipeline;

// The form a record answers to (Pack::find): a variant record's is its state's own
PackForm recordForm(const PackPipeline &pp);

constexpr uint8_t cbufKind(uint8_t type)
{
    return type & 7;
}

// A program's constant buffer of set 1, the heap's arrays (nvk_switch_capture.c switch_nvk_cbuf_code: the set in bits 3-7)
constexpr uint8_t kCbufHeapSet = CbufSet | 1 << 3;

constexpr uint32_t cbufUboOffset(uint8_t type)
{
    return uint32_t(type >> 3) * 16;
}

// Set-0 entries a state may have: Core Animation's generic states (the uber shader) take 27; and the bytes of its set 0,
// every entry's 16-byte descriptor at most (RenderEncoder::prepare's)
constexpr uint32_t kPackEntries = 32, kSetMaxBytes = 16 * kPackEntries;
// The fragment texture slots a record can bind (probe.mm's tagged textures), and so the slots a record names a written
// texture's format at
constexpr uint32_t kWrittenSlots = 16;
// A record's fixed-function vertex streams: stream 0 (vertexSlot) and PackPipeline::vertexStreams'
constexpr uint32_t kPackVertexStreams = 8;
// A vertexStreams byte's flag: the stream's stride is its bind's (MTLBufferLayoutStrideDynamic, a runtime record's alone)
constexpr uint8_t kStreamBindStride = 0x80;
// The largest stride a fixed-function vertex stream fetches at: NVB197_SET_VERTEX_STREAM_A_FORMAT_STRIDE is bits 11:0
constexpr uint32_t kStreamStrideField = 0xFFF;

struct PackShaderTests {
    uint8_t depthWord, stencilWord, flags, reserved;
    uint32_t depthTic, stencilTic;
    uint32_t reservedTail;
};

// One set-1 heap array of a state (m2v's argument-buffer textures and samplers, bind/heap.rs): its class as the bridge words
// it (metal2vulkan heap_class_code), and the byte of set 1 its element 0 is at in NVK's layout. Every element is 4 bytes
// (nvk_sampled_image_descriptor), element i the descriptor of texture header i (an image class) or sampler header i.
struct PackHeapArray {
    uint32_t heapClass, offset;
};

constexpr uint32_t kPackHeapArrays = 8;
// Every heap array's elements: the texture header pool's (Device kTicCount), which the sampler header pool's fits
constexpr uint32_t kPackHeapElements = 8192;

struct PackPipeline {
    uint64_t keyHash;    // FNV-1a 64 of the state key (the frontend journal's R-line fields but the label)
    uint32_t keyString;  // offset in the strings section: the key's fields before " | v" (attachments, vertex layout)
    uint32_t vs, fs;     // program indices
    uint32_t blob, blobDwords;  // (method, value) pairs in the blob section, dwords = 2 x pairs
    uint8_t entries, pushes, vertexSlot, pad;
    uint16_t vertexStride, setBytes;
    uint32_t flags;
    PackEntry entry[kPackEntries];
    uint32_t
        pushMask;  // root-table words [0x700, 0x780) the state's push ranges write (m2v's windows, as NVK wrote them)
    uint32_t pushWords[32];
    uint8_t blendColorWord, halfBlendColorWord;  // push word + 1, zero when absent
    uint16_t iterated;  // a fetch-free form's iterated blend: its PackIteratedBlend's index + 1; 0: none
    uint8_t storageMask, storageWriteMask, storageSamples, storageTests;
    uint32_t storageTic[8];
    PackShaderTests shaderTests;
    uint32_t variants;        // the first of its stages' variants in the variants section
    uint8_t variantCount[2];  // vertex, then fragment: the vertex stage's first
    // the storage attachments whose fixed-function blend the fragment program computes inside the interlock, and what its
    // model assumes of the ROP it stands in for (ropBlendAssumptions)
    uint8_t storageBlendMask, storageBlendModel;
    uint32_t boundEntries;                     // bit k: entry[k]'s handle word is read by a bound texture instruction
    PackUniformRange uniform[kUniformRanges];  // assumption bits 0-7
    uint8_t fetchRung[8];                      // per color attachment: PackFetchRung, candidate rungs (kFetchRungMask)
    uint32_t variantName;                      // a PipelineVariantRecord's name: its offset in the strings section
    // bit k: the GPU reads entry[k]'s words from the set's upload (raw_set_reads.h); a draw whose other entries alone
    // moved keeps the set it uploaded last
    uint32_t memoryEntries;
    // What m2v's reads through argument buffers take of a draw (raw_heap_tables.h): each stage's push word + 1 of the root
    // table's address and of the ExecutionStatus buffer's (0: none), and the buffer slots its root table holds
    // (bit n: [[buffer(n)]], m2v's BufferForm::Psb), vertex then fragment
    uint8_t rootTableWord[2], statusWord[2];
    uint32_t rootSlots[2];
    uint8_t heapArrays, heapReserved[3];
    uint32_t heapBytes;  // set 1's size in NVK's layout
    PackHeapArray heap[kPackHeapArrays];
    // m2v's ArgumentTable of each stage, vertex then fragment (raw_spill_table.h): its EntrySpillTable's index + 1 (0: none),
    // its words, and the first of them in the blob section (the vertex table's first, then the fragment table's)
    uint8_t spillEntry[2], spillWords[2];
    uint32_t spillAt;
    // v21. Up to arrayLengthWord and pulledStride, runtime records only (raw_direct.cpp; every pack state has these 0). A
    // tessellation state's
    // push word of its control stage's first field + 1 (raw_direct_push.h kControlWords; its control and evaluation
    // programs are RuntimeRecord's), its patches' control points (0: any) and its control-point index size in bytes (0:
    // none). vertexStrideDynamic: stream 0's stride is its bind's (MTLBufferLayoutStrideDynamic).
    uint8_t controlWord, controlPoints, controlPointIndexBytes, vertexStrideDynamic;
    // A state of vertex amplification: its maxVertexAmplificationCount (0: 1), and the push words + 1 of m2v's
    // AmplificationId and AmplificationCount (0: none), which each draw writes (its [[amplification_count]] is the draw's)
    uint8_t amplificationMax, amplificationIdWord, amplificationCountWord;
    // A pack field: the push word + 1 of m2v's RenderTargetArrayLength (0: none), the pass's length, which each draw writes:
    // its vertex program writes Layer (SET_RT_LAYER takes it), an index at or past the length as 0. kLayerDropped: its
    // vertex function chooses its slice, but the translation had no layered form of the state; its record draws layer 0
    uint8_t arrayLengthWord;
    // A pulled vertex buffer layout's stride its binds set (m2v VertexBufferStride of a dynamic layout): its Metal buffer
    // index and push word + 1 (0: none)
    uint8_t pulledStride[4][2];
    // The fixed-function vertex streams past stream 0, in stream order (raw_emit.cpp: densely, by Metal buffer index): each
    // its Metal buffer index + 1 (0: no more), with kStreamBindStride; their strides are the state's
    // SET_VERTEX_STREAM_A_FORMAT registers
    uint8_t vertexStreams[kPackVertexStreams - 1];
    // The push word + 1 of m2v's ViewportCount (0: none), the encoder's viewport count, which each draw writes: an index
    // its vertex program writes at or past the count is 0
    uint8_t viewportCountWord;
    // v22. By fragment texture slot, the MTLPixelFormat of the texture its programs write (or read and write) as a storage
    // image were compiled for (0: not one): m2v specialises a storage texture's accesses on the bound format (its channels'
    // widths, codes and conversion), so a draw binding another format draws the record of its " | wtex=" variant
    // (raw_written_textures.h)
    uint16_t writtenFormat[kWrittenSlots];
    // the fragment texture slots its programs read through a two-plane RGB10A8_2P_XR10 texture's header pair (raw_xr_pass.h):
    // the record of its " | xrs=<slots>" variant
    uint16_t xrSlots, xrReserved;
};

// PackPipeline::arrayLengthWord of a record of one layer whose vertex function chooses its slice
constexpr uint8_t kLayerDropped = 0xFF;

// Whether a record of `arrayLengthWord` is a slice-choosing state's layered form: made for a pass of layers, its storage
// images 2D arrays of the pass's layers (the bridge's TYPE_2D_ARRAY storage writes, raw_direct_storage.h)
inline bool layeredRecord(uint8_t arrayLengthWord)
{
    return arrayLengthWord && arrayLengthWord <= 32;
}

// A vertexStreams byte's Metal buffer index
inline uint8_t streamSlot(uint8_t stream)
{
    return uint8_t((stream & ~kStreamBindStride) - 1);
}

// A vertex stream of the record's takes its bind's stride
inline bool bindStrideStreams(const PackPipeline &r)
{
    for (uint8_t stream : r.vertexStreams)
        if (stream & kStreamBindStride)
            return true;
    return r.vertexSlot != 0xFF && r.vertexStrideDynamic;
}

inline bool tessellates(const PackPipeline &p)
{
    return p.controlWord != 0;
}

// A compute state at one threadgroup shape (runtime/raw_compute.h): NVK's work for the kernel m2v made of it, keyed by
// computeKeyHash. Its program is a PackProgram whose code starts its slot (no shader header); its constant buffers are the
// program's (PackProgram::cbufType, QMD slot c its cbuf c)
enum PackKernelEntryKind : uint8_t {
    KernelBuffer,
    KernelTexture,
    KernelSampler,
    // m2v's handle tables of argument-buffer textures and samplers: a storage buffer descriptor of the device's own
    // (EntryTextureTable, EntrySamplerTable)
    KernelTextureTable,
    KernelSamplerTable,
    KernelSpillTable  // m2v's ArgumentTable (EntrySpillTable)
};

// A word range of set 0 and the Metal argument it holds: a buffer's {address, range} descriptor (its range the bound bytes
// padded to 16, at most `window` bytes where the function proves it reads no further: m2v's descriptor_range), a texture's
// header index or a sampler's (tic | tsc << 20, either alone in a word or both)
struct PackKernelEntry {
    uint16_t offset;
    uint8_t kind;  // PackKernelEntryKind
    uint8_t slot;
    // buffers: kernelBufferWindow, kernelResidualWord; textures: the header's word 0 m2v bound (format, swizzle)
    uint32_t window;
};

// A buffer entry's window in bytes, 0 the whole buffer
inline uint32_t kernelBufferWindow(const PackKernelEntry &e)
{
    return e.window & 0xFFFFFF;
}

// A buffer entry's root push word (0x700 + 4 x word) that holds m2v's SlotBindOffset residual for its slot, -1 where the
// record names none (its kernel then takes only offsets on 16 bytes: raw_residual.h)
inline int kernelResidualWord(const PackKernelEntry &e)
{
    return int(e.window >> 24) - 1;
}

constexpr uint32_t kKernelEntries = 64;

struct PackKernel {
    uint64_t keyHash;
    uint32_t keyString;
    uint32_t program;  // index in the programs section
    uint8_t entries;
    int8_t regionWord;  // root push word of m2v's DispatchRegion field (two uvec4: base, nominal size, grid), -1: none
    uint32_t qmd[64];   // NVK's QMD (QMDV01_07), its program offset, grid and constant buffer addresses zero
    uint16_t setBytes;  // set 0's size in the root table's set address
    uint64_t readOnlyEntries;  // bit i: entry[i] is proven read-only; old records conservatively leave every bit zero
    uint32_t pushMask;         // root table [0x700, 0x780): the constant words m2v's push ranges write, and which
    uint32_t pushWords[32];
    // a kernel whose bind offsets spill past its push window binds more than 28 buffers (raw_spill_table.h)
    PackKernelEntry entry[kKernelEntries];
    // What m2v's reads through argument buffers take of a dispatch, as PackPipeline's for the kernel stage: the push word + 1
    // of the root table's address and of the ExecutionStatus buffer's (0: none), the buffer slots its root table holds, and
    // its set-1 heap arrays
    uint8_t rootTableWord, statusWord, heapArrays, argumentsReserved;
    uint32_t rootSlots, heapBytes;
    PackHeapArray heap[kPackHeapArrays];
    // its ArgumentTable as PackPipeline's: the KernelSpillTable entry's index + 1, its words, their first in the blob
    uint8_t spillEntry, spillWords;
    uint16_t spillReserved;
    uint32_t spillAt;
    // A stage-in kernel's (v21): the push word + 1 of m2v's StageInRegion field (origin and size, two uvec4; 0: none), and
    // each layout of a dynamic stride's Metal buffer index and StageInStride push word + 1, which each dispatch writes
    uint8_t stageInRegionWord;
    uint8_t stageInStride[4][2];
    uint8_t stageInReserved[3];
};

inline bool kernelEntryWrites(const PackKernel &kernel, uint32_t entry)
{
    return !(kernel.readOnlyEntries & (1ull << entry));
}

struct SplitCost {
    uint16_t version, reserved;
    uint32_t launchCycles, drainCycles;
    uint32_t prologueWarpCycles, prologueInstructions;
    uint32_t savedPixelInstructions, savedPixelCycles, frontierValues;
};

static_assert(sizeof(SplitCost) == 32);

// A split's prologue writes values the fragment stage reads through frontierCbuf. Version 1 uses SplitBatchHeader/Record:
// one lane per draw, CB0 the header and CB1 shared shader constants. Version 0 binds each draw separately: CB0 holds push
// words and frontierVA at 128, CB1..uboCount its UBOs, then shader constants. uboCbuf always names the fragment bindings.
struct PackSplit {
    uint32_t thresholdPixels;
    uint32_t whole, fs, prologue;
    uint16_t frontierBytes;
    uint8_t frontierCbuf, uboCount, shaderDataCbuf, prologueCbufs;
    uint8_t uboCbuf[8];
    uint16_t batchVersion, batchThreads;
    uint32_t qmd[64];  // the prologue's QMD (QMDV01_07), its program offset and constant buffer addresses zero
    SplitCost cost;
};

constexpr uint32_t kSplitParamsBytes = 256, kSplitFrontierAddress = 128;

struct PackInitMethod {
    uint16_t subc, mthd;
    uint32_t data, reloc;
};

enum PackReloc : uint32_t {
    RelocNone,
    RelocHeapHi,
    RelocHeapLo,
    RelocZeroHi,
    RelocZeroLo,
    RelocRootHi,
    RelocRootLo,
    RelocTicHi,
    RelocTicLo,
    RelocTicMax,
    RelocTscHi,
    RelocTscLo,
    RelocTscMax,
    RelocTicketsLo,
    RelocTicketsHi
};

#pragma pack(pop)

// What a context init's marked methods take in this process (PackReloc): the shader heap, the zero page, the root table (NVK's
// cb0), the texture and sampler header pools and their highest indices, and the pixel interlock's ticket buffer, which the
// init's root table names (nvk_root_descriptor_table.interlock_ticket_buffer)
struct InitAddresses {
    uint64_t heap, zero, root, tic, tsc;
    uint32_t ticMax, tscMax;
    uint64_t tickets;
};

uint32_t relocated(const PackInitMethod &m, const InitAddresses &a);
static_assert(sizeof(PackShaderTests) == 16, "shader test layout");
static_assert(sizeof(PackPipeline) ==
                  40 + kPackEntries * 8 + 4 + 32 * 4 + 4 + 36 + 16 + 56 + 4 + 92 + 24 + 2 * kWrittenSlots + 4,
              "pack layout");
static_assert(offsetof(PackPipeline, storageBlendMask) == 490 && offsetof(PackPipeline, fetchRung) == 528,
              "pack layout: packfile.py PIPELINE_V15");
static_assert(offsetof(PackPipeline, rootTableWord) == 544 && offsetof(PackPipeline, heap) == 564 &&
                  offsetof(PackPipeline, spillEntry) == 628,
              "pack layout: packfile.py PIPELINE_V20");
static_assert(offsetof(PackPipeline, controlWord) == 636 && offsetof(PackPipeline, arrayLengthWord) == 643 &&
                  offsetof(PackPipeline, vertexStreams) == 652 && offsetof(PackPipeline, viewportCountWord) == 659,
              "pack layout: packfile.py PIPELINE_V21");
static_assert(offsetof(PackPipeline, writtenFormat) == 660 && offsetof(PackPipeline, xrSlots) == 692,
              "pack layout: packfile.py PIPELINE_V22");
static_assert(sizeof(PackProgram) == 44, "pack layout");
static_assert(sizeof(PackVariant) == 8, "pack layout");
static_assert(sizeof(PackArchive) == 16, "pack layout");
static_assert(sizeof(PackComputeFacts) == 24, "pack layout: packfile.py compute_facts_bytes");
static_assert(sizeof(PackHeader) == 24 + 16 * PackSectionCount + 16, "pack layout: packfile.py pack_bytes header_size");
static_assert(sizeof(PackKernel) == 18 + 256 + 14 + 128 + 8 * kKernelEntries + 84 + 12, "pack layout");
static_assert(offsetof(PackKernel, stageInRegionWord) == 1012, "pack layout: packfile.py KERNEL_V21");
static_assert(sizeof(PackSplit) == 34 + 256 + 32, "pack layout");
static_assert(sizeof(PackIteratedBlend) == 32 + 48 + 4 * kIteratedProgramWords, "pack layout");

class Pack {
public:
    ~Pack();
    // `stack`: the plugin's own key; a pack of another build (another m2v, NVK or frontend) is refused. 0 takes any (host tests).
    bool load(const char *path, uint64_t stack, std::string &err);

    const PackHeader *header() const
    {
        return hdr_;
    }

    template <class T> const T *section(PackSectionId s) const
    {
        return reinterpret_cast<const T *>(base_ + hdr_->section[s].offset);
    }

    uint64_t bytes(PackSectionId s) const
    {
        return hdr_->section[s].bytes;
    }

    const PackPipeline *find(uint64_t keyHash, PackForm form = PackForm::Default) const;  // nullptr: not in the pack
    // the state's PipelineVariantRecords, and the one named `name` (variantNameAt) of `form` (a two-plane form's storage
    // form has its barrier twin, raw_xr_pass.h); nullptr: not in the pack
    std::vector<const PackPipeline *> variantRecords(uint64_t keyHash) const;
    const PackPipeline *variantRecord(uint64_t keyHash, const std::string &name,
                                      PackForm form = PackForm::Default) const;
    const PackKernel *findKernel(uint64_t keyHash) const;  // nullptr: not in the pack
    const PackSplit *split(uint32_t wholeProgram) const;   // nullptr: not split
    // `pp`'s variants of stage `stage` (0 vertex, 1 fragment), `count` of them; nullptr: none
    const PackVariant *variants(const PackPipeline &pp, uint32_t stage, uint32_t &count) const;
    const PackArchive *archive(uint64_t pathHash) const;              // nullptr: the pack lists no such archive
    const PackIteratedBlend *iterated(const PackPipeline &pp) const;  // nullptr: it names none
    // `pp`'s or `k`'s ArgumentTable words (PackPipeline::spillAt); nullptr: none, or past the section
    const uint32_t *spills(const PackPipeline &pp) const;
    const uint32_t *spills(const PackKernel &k) const;
    bool declares(const PackArchive &archive, uint64_t keyHash) const;
    const PackComputeFacts *computeFacts(uint64_t keyHash) const;  // nullptr: the pack has no facts of that state

    const char *string(uint32_t off) const
    {
        return section<char>(PackStrings) + off;
    }

    static uint64_t fnv(const void *p, size_t n);

private:
    const uint8_t *base_ = nullptr;
    size_t size_ = 0;
    const PackHeader *hdr_ = nullptr;
    // the pipelines in keyHash order, for a pack whose section is not in that order itself (mkpack.py's are); empty: it is
    std::vector<uint32_t> index_;
};

}  // namespace raw
