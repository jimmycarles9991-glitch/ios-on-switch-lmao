// SPDX-License-Identifier: MIT
// The raw GM20B path's pipeline pack (pack/mkpack.py writes it, pack/rawpack.py runs the whole generation at bundle build time;
// this is its layout). Little endian, packed, sections 256-byte aligned. Read-only after load: mmapped shared and pointed into,
// so every process reads the same page-cache pages. Its pipelines are in keyHash order (Pack::find searches them in place).
#pragma once
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
    PackSectionCount
};

constexpr uint32_t kPackVersion = 8;

struct PackHeader {
    char magic[8];                                    // "GM20BPK1"
    uint32_t version, programs, pipelines, samplers;  // kPackVersion
    PackSection section[PackSectionCount];
    uint64_t stack;  // the build's stack key (build-tegra-gm20b-metal.py: the plugin is compiled with the same one)
};

// A program's constant buffer as the pack and the runtime's capture name it: its nvk_cbuf_type in bits 0-2, and for a uniform
// buffer's descriptor (CbufUbo) its byte offset in set 0 / 16 in bits 3-7. A uniform buffer of another set, or past 496 bytes,
// is CbufUnbindable: no raw draw binds it.
enum PackCbufType : uint8_t {
    CbufRoot = 1,
    CbufShaderData = 2,
    CbufSet = 3,
    CbufUbo = 5,
    CbufUnbindable = 7
};

struct PackProgram {
    uint32_t heapOffset;  // slot start in the heap image
    uint32_t program;     // SET_PIPELINE_PROGRAM value (header at heapOffset + 0x30, code at + 0x80)
    uint32_t bytes;       // slot size
    // NAK's shader data in the slot (where NVK uploads it: 256-aligned after the code) and its size, a multiple of 256; 0: none
    uint32_t dataOffset, dataBytes;
    uint8_t cbufs;         // constant buffer slots it uses
    uint8_t cbufType[11];  // each slot's PackCbufType: the root table at 0
};

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
    EntryColorTable = 5
};

constexpr uint8_t kNoSlot = 0xFF;

enum PackSamplerKind : uint8_t {
    SamplerNone = 0,
    SamplerSlot = 1,
    SamplerConstexpr = 2
};

struct PackEntry {
    uint16_t offset;      // byte in the 256-byte set 0
    uint8_t kind;         // PackEntryKind
    uint8_t stage;        // UBO: 1 vertex, 2 fragment; handles: 2
    uint8_t slot;         // UBO: Metal buffer index; texture: fragment texture index; fetch: color attachment
    uint8_t samplerKind;  // handles: PackSamplerKind. Buffers: bufferStorage, bufferResidualWord
    uint8_t sampler;      // handles: fragment sampler index, or constexpr sampler number. Buffers: the function's
    uint8_t pad;          //   window on the slot in 16-byte units (sampler | pad << 8), 0 = the whole buffer
};

// A buffer entry: storage, else uniform (NVK rounds its range up to the 256-byte cbuf alignment)
inline bool bufferStorage(const PackEntry &e)
{
    return e.samplerKind & 1;
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
    PipelineBarrierForm = 2
};

// Which of a state's records: its first (the storage form where it has both), or its barrier form
enum class PackForm : uint8_t {
    Default,
    Barrier
};

constexpr uint8_t cbufKind(uint8_t type)
{
    return type & 7;
}

constexpr uint32_t cbufUboOffset(uint8_t type)
{
    return uint32_t(type >> 3) * 16;
}

// Set-0 entries a state may have: Core Animation's generic states (the uber shader) take 27
constexpr uint32_t kPackEntries = 32;

struct PackPipeline {
    uint64_t keyHash;    // FNV-1a 64 of the state key (the frontend journal's R-line fields but the label)
    uint32_t keyString;  // offset in the strings section: the key's fields before " | v" (attachments, vertex layout)
    uint16_t vs, fs;     // program indices
    uint32_t blob, blobDwords;  // (method, value) pairs in the blob section, dwords = 2 x pairs
    uint8_t entries, pushes, vertexSlot, pad;
    uint16_t vertexStride, setBytes;
    uint32_t flags;
    PackEntry entry[kPackEntries];
    uint32_t
        pushMask;  // root-table words [0x700, 0x780) the state's push ranges write (m2v's windows, as NVK wrote them)
    uint32_t pushWords[32];
};

// A compute state at one threadgroup shape (runtime/raw_compute.h): NVK's work for the kernel m2v made of it, keyed by
// computeKeyHash. Its program is a PackProgram whose code starts its slot (no shader header); its constant buffers are the
// program's (PackProgram::cbufType, QMD slot c its cbuf c)
enum PackKernelEntryKind : uint8_t {
    KernelBuffer,
    KernelTexture,
    KernelSampler
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

struct PackKernel {
    uint64_t keyHash;
    uint32_t keyString;
    uint16_t program;  // index in the programs section
    uint8_t entries;
    int8_t regionWord;  // root push word of m2v's DispatchRegion field (two uvec4: base, nominal size, grid), -1: none
    uint32_t qmd[64];   // NVK's QMD (QMDV01_07), its program offset, grid and constant buffer addresses zero
    uint16_t setBytes;  // set 0's size in the root table's set address
    uint16_t pad;
    uint32_t pushMask;  // root table [0x700, 0x780): the constant words m2v's push ranges write, and which
    uint32_t pushWords[32];
    PackKernelEntry entry[16];
};

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
    RelocTscMax
};

#pragma pack(pop)

// What a context init's marked methods take in this process (PackReloc): the shader heap, the zero page, the root table (NVK's
// cb0), the texture and sampler header pools and their highest indices
struct InitAddresses {
    uint64_t heap, zero, root, tic, tsc;
    uint32_t ticMax, tscMax;
};

uint32_t relocated(const PackInitMethod &m, const InitAddresses &a);
static_assert(sizeof(PackPipeline) == 36 + kPackEntries * 8 + 4 + 32 * 4, "pack layout");
static_assert(sizeof(PackProgram) == 32, "pack layout");
static_assert(sizeof(PackKernel) == 16 + 256 + 8 + 128 + 128, "pack layout");

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
    const PackKernel *findKernel(uint64_t keyHash) const;                                 // nullptr: not in the pack

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
