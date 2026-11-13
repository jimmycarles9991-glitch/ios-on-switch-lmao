// SPDX-License-Identifier: MIT
// What one command buffer recorded, as plain data the planner reads: the recording IR. It holds what RMCommandPlan and
// the RM objects hold today, with recording slots in place of object pointers. Draw counts, offsets, clear values and
// uniform bytes are lowering parameters: no planner decision reads them.
#pragma once
#include "plan_types.h"
#include <array>
#include <vector>

namespace raw::plan {

enum class Layout : uint8_t {
    Pitch,
    BlockLinear
};

struct ResourceFacts {
    enum Kind : uint8_t {
        Buffer,
        Texture,
        Sampler,
        Event
    };

    enum Flag : uint32_t {
        Memoryless = 1u << 0,
        Compressed = 1u << 1,  // a compressible kind (raw_comp.h)
        Surface = 1u << 2,     // an IOSurface's memory
        Drawable = 1u << 3,    // a CAMetalLayer drawable
        HostVisible = 1u << 4,
        SampledShadow = 1u << 5,  // sampled through a pool image (raw::Texture::sampled), by passes and kernels alike
        SurfaceView = 1u << 6,    // a shadowed surface view: its image is refreshed at every pass that samples it
        ExtendedRange = 1u << 7,  // drawn through an RGBA16F stand-in (raw_xr.h)
        PlanarStandIn = 1u << 8,  // a planar render target drawn through a stand-in (raw::Texture::planarStandIn)
        PlanarCodes = 1u << 9,    // a 4:4:4 code texture over two planes (raw::Texture::planar)
        Ycbcr = 1u << 10,         // a two-plane YCbCr texture sampled through a converted image (raw_ycbcr.h)
        ImageStale = 1u << 11,    // its converted image is behind its planes (surface seed, writes, native generation)
        MipChain2D = 1u << 12,    // its chain's levels are the 2D engine's (raw::Texture::chain->fmt2d)
        RotatedPlane = 1u << 14,  // a drawable on the rotated (landscape) plane
        TwoPlaneXr = 1u << 15,    // RGB10A8_2P_XR10 over Metal's two planes
        StoredMultisample = 1u << 16,
        PlanarTarget = 1u << 17,        // a planar YCbCr colour attachment
        UnsupportedSampler = 1u << 18,  // an address mode or border the sampler header cannot express
        Rendered = 1u << 19,            // created with MTLTextureUsageRenderTarget
    };

    Kind kind = Texture;
    Handle handle;
    uint32_t memory =
        kNone;  // the backing the bytes lie in: views, buffer textures, shared surfaces, aliased memoryless
    uint64_t offset = 0, bytes = 0;
    uint32_t flags = 0;
    uint16_t format = 0;  // MTLPixelFormat
    uint16_t levels = 1, layers = 1;
    uint8_t samples = 1;
    Layout layout = Layout::Pitch;
    uint32_t width = 0, height = 0;
    uint32_t shadow = kNone;   // its pool image (SampledShadow)
    uint32_t standIn = kNone;  // ExtendedRange's or PlanarStandIn's stand-in
    uint32_t luma = kNone, chroma = kNone;

    bool has(Flag f) const
    {
        return flags & f;
    }
};

// What the record (L4) says of a pipeline: masks over colour attachments, the fetch rung per colour as record v15 stores
// it (raw_pack.h PackFetchRung low nibble; candidate bits 0x10 blend, 0x20 iterated), and the forms it was made in
struct ProgramFacts {
    enum Form : uint8_t {
        Whole = 1,
        Split = 2,
        StorageForm = 4,
        BarrierForm = 8
    };

    uint64_t key = 0;
    std::array<uint16_t, 8> colorFormats{};
    uint16_t depthFormat = 0, stencilFormat = 0;
    uint8_t samples = 1;
    uint8_t writeMask = 0, readMask = 0, fetchMask = 0, storageMask = 0, storageWriteMask = 0;
    std::array<uint8_t, 8> fetchRung{};
    uint8_t forms = Whole;
    bool primitiveId = false, discards = false, compute = false;
};

enum class Load : uint8_t {
    DontCare,
    Load,
    Clear
};
enum class Store : uint8_t {
    DontCare,
    Store,
    Resolve,
    StoreAndResolve
};

struct Attachment {
    uint32_t texture = kNone;
    uint16_t level = 0, slice = 0, depthPlane = 0;
    Load load = Load::DontCare;
    Store store = Store::DontCare;
    uint32_t resolve = kNone;
    uint16_t resolveLevel = 0, resolveSlice = 0;
    std::array<float, 4> clear{};
};

struct PassDesc {
    std::array<Attachment, 8> color{};
    Attachment depth, stencil;
    double clearDepth = 1;
    uint32_t clearStencil = 0;
    uint32_t width = 0, height = 0;  // renderTargetWidth/Height, 0 where unset
    uint16_t layers = 0;             // renderTargetArrayLength
    bool rateMap = false, samplePositions = false;
};

// A resource a state binds: its stage, its slot in that stage's table and the path its program reads or writes it by
// (the record's entry kind: a uniform entry is Constant, a storage buffer or image Storage, a texture Texture)
struct Binding {
    uint32_t resource = kNone;
    uint8_t stage = 0;  // 0 vertex, 1 fragment, 2 kernel
    uint8_t index = 0;
    Path path = Path::Texture;
    bool write = false;
    uint64_t offset = 0;
};

struct Span {
    uint32_t first = 0, count = 0;
};

struct DrawState {
    uint32_t program = kNone;
    Span bindings;
    bool depthWrite = false, stencilWrite = false, depthTest = false;
    bool visibility = false;
};

struct Draw {
    uint32_t state = kNone;
    uint8_t primitive = 3;  // MTLPrimitiveType
    uint8_t indexSize = 0;  // 0: not indexed
    bool indirect = false;
    uint32_t indices = kNone, arguments = kNone;
    uint32_t first = 0, count = 0, instances = 1, baseInstance = 0;
    int32_t baseVertex = 0;
    uint64_t indexOffset = 0, argumentOffset = 0;
};

struct Dispatch {
    uint32_t program = kNone;
    Span bindings;
    std::array<uint64_t, 3> grid{};
    std::array<uint32_t, 3> threads{};
    bool perThread = false;
    uint32_t imageShadowSlots =
        0;  // texture slots sampled through an image of their own (RMPlanDispatch::surfaceSlots)
};

struct Transfer {
    enum Kind : uint8_t {
        CopyTexture,
        CopyBuffer,
        CopyBufferToTexture,
        CopyTextureToBuffer,
        FillBuffer,
        Mipmaps
    };

    Kind kind = CopyBuffer;
    uint32_t source = kNone, destination = kNone;
    SubRange sourceRange, destinationRange;
    uint8_t aspect = 0;  // MTLBlitOption depth/stencil from a combined format
    uint8_t value = 0;
    uint64_t sourceOffset = 0, destinationOffset = 0, bytes = 0;
    std::array<uint32_t, 3> origin{}, size{};
};

struct Control {
    enum Kind : uint8_t {
        MemoryBarrier,
        UseResource,
        UpdateFence,
        WaitFence
    };

    enum Scope : uint8_t {
        Buffers = 1,
        Textures = 2,
        RenderTargets = 4
    };

    Kind kind = MemoryBarrier;
    uint8_t scope = 0;
    uint8_t after = 0, before = 0;  // MTLRenderStages
    uint32_t resource = kNone;
    bool write = false;
};

struct Event {
    uint32_t event = kNone;
    uint64_t value = 0;
    bool signal = false, shared = false;
};

enum class OpKind : uint8_t {
    Draw,
    Dispatch,
    Transfer,
    Control
};

struct Op {
    OpKind kind = OpKind::Draw;
    uint32_t payload = 0;
};

enum class EncoderKind : uint8_t {
    Render,
    Blit,
    Compute
};

struct Encoder {
    EncoderKind kind = EncoderKind::Render;
    uint32_t pass = kNone;
    Span ops;
    bool serial = true;
};

// A point between encoders: a wait or signal of an event, or the present of a drawable
struct Boundary {
    enum Kind : uint8_t {
        Wait,
        Signal,
        Present
    };

    Kind kind = Wait;
    uint32_t afterEncoder = 0;  // placed after this many encoders
    uint32_t payload = 0;       // Event index, or the drawable's slot
};

struct Recording {
    std::vector<ResourceFacts> resources;
    std::vector<ProgramFacts> programs;
    std::vector<PassDesc> passes;
    std::vector<Encoder> encoders;
    std::vector<Op> ops;
    std::vector<DrawState> states;
    std::vector<Binding> bindings;
    std::vector<Draw> draws;
    std::vector<Dispatch> dispatches;
    std::vector<Transfer> transfers;
    std::vector<Control> controls;
    std::vector<Event> events;
    std::vector<Boundary> boundaries;
};

}  // namespace raw::plan
