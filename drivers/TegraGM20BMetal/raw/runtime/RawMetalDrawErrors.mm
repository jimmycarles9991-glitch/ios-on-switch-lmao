// SPDX-License-Identifier: MIT
// Draws raw::RenderEncoder refuses that go nowhere else: those Metal fails as well (each line names the Metal header or
// error that defines the failure), records breaking what their making guarantees (the driver's error) and draws the raw
// path has no form of yet. Each logs its own line and fails its command buffer, which then submits nothing
// (RawMetalSubmit.mm finishes it with its error before anything could replay it elsewhere).
#import "RawMetalInternal.h"

// raw_direct.cpp turnedVariant sets PipelineTurnRefused where NVK's turned compile of the fragment stage failed, or
// where it differs from the generic compile in its cbuf types, early tests, interlock or sample shading. The second
// cannot happen: the turn (nvk_switch_turn.c) adds only cbuf 0 reads, derivative clones and an interpolation at a
// sample as one at its offset; early tests and sample shading come from the SPIR-V (spirv_to_nir.c), the interlock mode
// from those, and cbuf 0 is always the root table, the others the descriptors the program reads
// (nvk_nir_lower_descriptors.c build_cbuf_map). So the draw is one of a pipeline that could not be compiled.
void rmTurnRefused(RMCommandBuffer *cb, uint64_t keyHash, uint32_t turn, const char *key)
{
    SWITCH_REFUSE_DETAIL("raw", "pass.turn_refused", keyHash,
                         "state %016llx into a plane of rotation %u: no turned fragment program (NVK failed its turned "
                         "compile, which differs from the generic in no field a record holds): "
                         "MTLCommandBufferErrorInternal",
                         (unsigned long long)keyHash, turn);
    failCommandBuffer(cb,
                      [NSString stringWithFormat:@"A state drawn into a turned drawable whose turned fragment program "
                                                 @"could not be compiled (state %016llx: %s)",
                                                 (unsigned long long)keyHash, key ?: "its key unknown here"]);
}

bool drawFailed(RMCommandBuffer *cb, const raw::Refusal &f, const char *why)
{
    MTLCommandBufferError code = MTLCommandBufferErrorInternal;
    switch (f.id) {
    case raw::RefTessellationDraw:
        SWITCH_REFUSE(
            "raw", "draw.tessellation",
            "%s (primitive %u, the state tessellates: %u): Metal's validation error (MTLRenderCommandEncoder.h: "
            "drawPatches: needs a postTessellationVertexFunction, drawPrimitives: a normal vertexFunction)",
            why, f.a, f.b);
        break;
    case raw::RefAmplificationCount:
        SWITCH_REFUSE(
            "raw", "draw.amplification_count",
            "%s (%u of %u): Metal's validation error (MTLRenderCommandEncoder.h "
            "setVertexAmplificationCount:viewMappings:, at most MTLRenderPipeline.h maxVertexAmplificationCount)",
            why, f.a, f.b);
        break;
    case raw::RefHeapMemory:
        SWITCH_REFUSE("raw", "draw.heap_memory", "%s: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)", why);
        code = MTLCommandBufferErrorOutOfMemory;
        break;
    case raw::RefTessellationScale:
        SWITCH_REFUSE("raw", "draw.tessellation_scale",
                      "%s: Metal's validation error (MTLRenderCommandEncoder.h setTessellationFactorScale:, a normal "
                      "float above 0)",
                      why);
        break;
    case raw::RefControlPointOffset:
        SWITCH_REFUSE("raw", "draw.control_point_offset",
                      "%s (offset %u of %u-byte indices): Metal's validation error (MTLRenderCommandEncoder.h "
                      "drawIndexedPatches: controlPointIndexBufferOffset, a multiple of 4 bytes)",
                      why, f.a, f.b);
        break;
    case raw::RefIndirectArguments:
        SWITCH_REFUSE("raw", "draw.indirect_arguments",
                      "%s (offset %u of %u bytes): Metal's validation error (MTLRenderCommandEncoder.h "
                      "indirectBufferOffset, a multiple of 4 bytes with the arguments inside the buffer)",
                      why, f.a, f.b);
        break;
    case raw::RefIndirectIndexOffset:
        SWITCH_REFUSE("raw", "draw.indirect_index_offset",
                      "%s (offset %u, %u-byte indices): Metal's validation error (MTLRenderCommandEncoder.h "
                      "drawIndexedPrimitives:indexType:indexBuffer:indexBufferOffset:indirectBuffer:, an offset of a "
                      "multiple of 4 bytes)",
                      why, f.a, f.b);
        break;
    case raw::RefCommandsRange:
        SWITCH_REFUSE("raw", "draw.commands_range",
                      "%s (commands %u + %u of %u): Metal's validation error (MTLRenderCommandEncoder.h "
                      "executeCommandsInBuffer:withRange:, a range inside the indirect command buffer)",
                      why, f.a, f.b, f.c);
        break;
    // what the records' making guarantees (raw_record_parts.cpp assembleRecord, the bridge's placements, the variant a
    // draw takes): a record that breaks it is the driver's error
    case raw::RefStreamSlot:
        SWITCH_REFUSE("raw", "draw.stream_slot",
                      "%s (buffer %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a vertex descriptor's "
                      "buffers are 0 to 30 (MTLVertexDescriptor.h)",
                      why, f.a);
        break;
    case raw::RefClipControl:
        SWITCH_REFUSE("raw", "draw.clip_control",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); every record's registers set the clip "
                      "control (raw_emit.cpp)",
                      why);
        break;
    case raw::RefCommandsRangeBuffer:
        SWITCH_REFUSE("raw", "draw.commands_range_buffer",
                      "%s (offset %u of %u bytes): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the recording "
                      "runs no execute whose range buffer is so (raw:icb.range)",
                      why, f.a, f.b);
        break;
    case raw::RefPatchCommands:
        SWITCH_REFUSE("raw", "draw.patch_commands_set_address",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the control stage m2v synthesizes reads "
                      "set 0 through three storage buffers, which NVK maps to a constant buffer",
                      why);
        break;
    case raw::RefLayerDropped:
        SWITCH_REFUSE(
            "raw", "draw.layer_dropped",
            "%s (%u layers): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the bridge makes a one-layer "
            "record of a slice-choosing state (LAYER_DROPPED) only of a multisampled storage form, and no "
            "layered pass is multisampled (raw_layers.h)",
            why, f.a);
        break;
    case raw::RefTurnRefused:
        rmTurnRefused(cb, uint64_t(f.b) << 32 | f.a, f.c, nullptr);
        return true;
    case raw::RefUnnamedUniform:
        SWITCH_REFUSE("raw", "draw.ubo_unnamed",
                      "%s: MTLCommandBufferErrorInternal (MTLCommandBuffer.h); assembleRecord refuses such a record",
                      why);
        break;
    case raw::RefSpillTable:
        SWITCH_REFUSE("raw", "draw.spill_table",
                      "%s (stage %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a record's argument tables "
                      "come with it, and a state with them is offered no split",
                      why, f.a);
        break;
    case raw::RefSetBytes:
        SWITCH_REFUSE("raw", "draw.set_bytes",
                      "%s (%u entries): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); Pack::load and addRecord "
                      "refuse a record whose far block does not hold its entries",
                      why, f.a);
        break;
    case raw::RefViewports:
        SWITCH_REFUSE("raw", "draw.viewports",
                      "%s (%u viewports): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); records since pack v21 "
                      "name the viewport count word",
                      why, f.a);
        break;
    case raw::RefWrittenTexture:
        SWITCH_REFUSE("raw", "draw.written_texture",
                      "%s (slot %u, bound %u, stored %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); a draw "
                      "takes the variant made for its written textures' formats",
                      why, f.a, f.b, f.c);
        break;
    case raw::RefWrittenPlanes:
        SWITCH_REFUSE("raw", "draw.written_planes",
                      "%s (slot %u): MTLCommandBufferErrorInternal (MTLCommandBuffer.h); the pass images every "
                      "two-plane texture its draws write",
                      why, f.a);
        break;
    // draws Metal takes that the raw path has no form of yet: never handed to m2v either
    case raw::RefStrideTwin:
        SWITCH_REFUSE("raw", "draw.stride_twin", "%s: the raw path has no form of it yet; the command buffer fails",
                      why);
        break;
    default:
        return false;
    }
    failCommandBuffer(cb, @(why), code);
    return true;
}
