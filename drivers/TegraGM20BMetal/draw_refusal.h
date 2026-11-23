// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace gm20b {

// Which encode-time predicate refused a draw (set beside *why; TegraGM20BMetal.mm counts and logs it, r10-rawsweep diagnostics)
enum RefusalId : uint8_t {
    RefNone,
    RefIndexedRange,
    RefColorFormats,
    RefNoTic,
    RefDepthStencil,  // a shader test the pass's attachment lacks: a: its format, b: the record's tests, c: samples
    RefSamples,       // a: the state's samples, b: the pass's, c: the state's shading passes
    RefGeometry,      // a patch draw of a form fetching through the texture unit: a primitive, b patches, c instances
    RefStorageView,   // a: the attachment, b: its format, c: its width (storage_form.h)
    RefRotated,       // a: why (rotated_pass.h RotatedRefusal), b: the rotation
    // from here on draws Metal fails too: their command buffer fails with Metal's error (TegraGM20BMetalDrawErrors.mm drawFailed)
    RefViewports,           // a: the encoder's viewport count
    RefWrittenTexture,      // a: the slot, b: the bound format, c: the format the record's programs store
    RefTessellationDraw,    // a: the draw's primitive (icb::kPatchPrimitive: patches), b: 1 where the state tessellates
    RefAmplificationCount,  // a: the encoder's amplification count, b: the state's maxVertexAmplificationCount
    RefHeapMemory,          // no memory for the tables the state reads argument buffers through
    RefUnnamedUniform,
    RefSpillTable,     // a: the stage (1 vertex, 2 fragment) whose argument table words the record lacks
    RefWrittenPlanes,  // a: the slot of a written two-plane texture the pass holds no image of
    RefTessellationScale,
    RefControlPointOffset,   // a: the control-point index buffer's offset, b: its index size
    RefIndirectArguments,    // a: the arguments' offset, b: their buffer's length
    RefIndirectIndexOffset,  // a: the index buffer's offset, b: the index size
    RefCommandsRange,        // a: the first command, b: the count, c: the commands the buffer holds
    RefStreamSlot,           // a: the stream's Metal buffer index
    RefClipControl,          // a depth clamp on a state whose registers set no clip control
    RefCommandsRangeBuffer,  // a: the execute range's offset, b: its buffer's length
    RefTurnRefused,          // a, b: the record's keyHash, low then high word; c: the rotation
    RefPatchCommands,        // patch commands of a state whose control stage reads set 0 by its address
    RefLayerDropped,         // a: the pass's layers
    // draws Metal takes that the driver has no form of yet: their command buffer fails too
    RefStrideTwin,  // a bind stride past a stream's 12-bit field, of a state with no stride twin
    // a shader depth/stencil test's: no memory for its stand-ins or no header for their view (OutOfMemory); a draw
    // breaking what its variant's choice and its pass's hidden attachment guarantee (Internal). a, b, c: as RefDepthStencil
    RefTestsMemory,
    RefTestsInvariant,
    RefViewHeader,  // no header left for a framebuffer fetch's view: a: the attachment, b: its format, c: its width
    // a storage form drawn into an attachment of another sample count than its state's (Metal's validation error): a:
    // the attachment, b: its format, c: its samples
    RefStorageSamples,
    RefSamplerHeader,  // no header the range makes: a: the sampler, b: the address map, c: 1 compare, 2 constexpr
    RefMatrixUniform,  // a colour matrix form's uniform not bound whole: a: its buffer index, b: offset, c: bytes bound
    RefNoStandIn  // a client's draw sampling a texture of no stand-in that a draw reads through one: a: slot, b: format
};

struct Refusal {
    uint8_t id = RefNone;
    uint32_t a = 0, b = 0, c = 0;
};

}  // namespace gm20b
