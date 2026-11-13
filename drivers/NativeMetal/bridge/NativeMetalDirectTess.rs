// SPDX-License-Identifier: MIT
//! A tessellation state's probe draw for the raw path's direct compile (NativeMetalDirect.rs): four patches of the
//! function's control points, read through a bound factor buffer (the raw path binds its zero buffer where the encoder set
//! none, as m2v's binder does).

use metal2vulkan::bind::{BindState, DrawCall, IndexBufferRef, TessellationFactorBuffer, TessellationFactors};
use metal2vulkan::metal::TessellationControlPointIndexType;
use metal2vulkan::pipeline::TessellationState;

const PROBE_PATCHES: u32 = 4;
const PROBE_FACTOR_BYTES: u64 = 4096;
const PROBE_INDEX_BYTES: u64 = 4096;

/// The control-point index size of the state's patches, in bytes (0: none)
pub(crate) fn index_bytes(tess: &TessellationState) -> u32 {
    match tess.control_point_index_type {
        TessellationControlPointIndexType::UInt16 => 2,
        TessellationControlPointIndexType::UInt32 => 4,
        _ => 0,
    }
}

/// The probe's draw of a state: four vertices of primitives, or four patches of a tessellation state (indexed where it has a
/// control-point index type)
pub(crate) fn probe_draw(tess: Option<&TessellationState>) -> DrawCall {
    let Some(t) = tess else {
        return DrawCall::Primitives {
            vertex_start: 0,
            vertex_count: 4,
            instance_count: 1,
            base_instance: 0,
        };
    };
    DrawCall::Patches {
        control_points: t.control_points.max(1),
        patch_start: 0,
        patch_count: PROBE_PATCHES,
        patch_index: None,
        instance_count: 1,
        base_instance: 0,
        control_point_index: (index_bytes(t) != 0).then(|| (IndexBufferRef::new(1, PROBE_INDEX_BYTES), 0)),
    }
}

/// The probe's factor buffer, which a tessellation state's draws read (the raw path binds its zero buffer where the encoder
/// set none)
pub(crate) fn bind_factors(state: &mut BindState) {
    state.tessellation_factors =
        TessellationFactors::new(Some(TessellationFactorBuffer::new(PROBE_FACTOR_BYTES, 0, 0)), 1.0);
}
