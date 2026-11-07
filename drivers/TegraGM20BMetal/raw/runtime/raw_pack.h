// SPDX-License-Identifier: MIT
// The raw GM20B path's pipeline pack (pack/mkpack.py writes it, pack/rawpack.py runs the whole generation at bundle build time;
// this is its layout). Little endian, packed, sections 256-byte aligned. Read-only after load: mmapped and pointed into.
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
    PackSectionCount
};

struct PackHeader {
    char magic[8];                                    // "GM20BPK1"
    uint32_t version, programs, pipelines, samplers;  // version 4
    PackSection section[PackSectionCount];
    uint64_t stack;  // the build's stack key (build-tegra-gm20b-metal.py: the plugin is compiled with the same one)
};

// NVK's constant buffer types (nvk_shader.h nvk_cbuf_type) a raw draw binds
enum PackCbufType : uint8_t {
    CbufRoot = 1,
    CbufShaderData = 2,
    CbufSet = 3
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
    EntryFetch = 3
};

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
    uint8_t samplerKind;  // handles: PackSamplerKind. Buffers: 0 uniform (NVK rounds the range up to 256), 1 storage
    uint8_t sampler;      // handles: fragment sampler index, or constexpr sampler number. Buffers: the function's
    uint8_t pad;          //   window on the slot in 16-byte units (sampler | pad << 8), 0 = the whole buffer
};

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

struct PackPipeline {
    uint64_t keyHash;    // FNV-1a 64 of the state key (the frontend journal's R-line fields but the label)
    uint32_t keyString;  // offset in the strings section: the key's fields before " | v" (attachments, vertex layout)
    uint16_t vs, fs;     // program indices
    uint32_t blob, blobDwords;  // (method, value) pairs in the blob section, dwords = 2 x pairs
    uint8_t entries, pushes, vertexSlot, pad;
    uint16_t vertexStride, setBytes;
    uint32_t flags;
    PackEntry entry[16];
    uint32_t
        pushMask;  // root-table words [0x700, 0x780) the state's push ranges write (m2v's windows, as NVK wrote them)
    uint32_t pushWords[32];
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
static_assert(sizeof(PackPipeline) == 36 + 16 * 8 + 4 + 32 * 4, "pack layout");
static_assert(sizeof(PackProgram) == 32, "pack layout");

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

    const char *string(uint32_t off) const
    {
        return section<char>(PackStrings) + off;
    }

    static uint64_t fnv(const void *p, size_t n);

private:
    const uint8_t *base_ = nullptr;
    size_t size_ = 0;
    const PackHeader *hdr_ = nullptr;
    std::vector<std::pair<uint64_t, uint32_t>> index_;  // sorted (keyHash, pipeline)
};

}  // namespace raw
