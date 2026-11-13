// SPDX-License-Identifier: MIT
//! A function's argument buffer as the raw path's encoder places its arguments (drivers/TegraGM20BMetal/raw/runtime
//! raw_argument_layout.h): m2v's layout of it (metal2vulkan bind/argument_buffer.rs, from the function's AIR) as flat runs
//! of leaf arguments (bind/argument_runs.rs), the layouts of the argument buffers its pointers point to after it.

use crate::objects::{NmbFunction, e, get, guard};
use metal2vulkan::bind::{ArgumentRun, ArgumentType, FlatLayout, flat_layouts};
use std::ffi::c_void;

/// raw_argument_layout.h ArgumentRun
#[repr(C)]
pub struct NmbArgumentRun {
    pub id: u32,
    pub count: u32,
    pub ids_per_element: u32,
    pub width: u32,
    pub kind: u32,
    pub nested: u32,
    pub offset: u64,
    pub stride: u64,
    pub element_stride: u64,
    pub size: u64,
}

#[repr(C)]
pub struct NmbArgumentLayout {
    pub encoded_length: u64,
    pub alignment: u64,
    pub runs: *const NmbArgumentRun,
    pub run_count: u32,
    pub reserved: u32,
}

/// raw_argument_layout.h ArgumentKind
fn kind_of(ty: ArgumentType) -> Result<u32, String> {
    Ok(match ty {
        ArgumentType::Buffer => 0,
        ArgumentType::Texture => 1,
        ArgumentType::Sampler => 2,
        ArgumentType::Constant => 3,
        ArgumentType::IndirectCommandBuffer => 4,
        ArgumentType::ComputePipelineState => 5,
        ArgumentType::RenderPipelineState => 6,
        ArgumentType::VisibleFunctionTable => 7,
        ArgumentType::IntersectionFunctionTable => 8,
        ArgumentType::AccelerationStructure => 9,
        ArgumentType::DepthStencilState => 10,
        other => return Err(format!("an argument of type {other:?} in a flat run")),
    })
}

fn run_of(r: &ArgumentRun) -> Result<NmbArgumentRun, String> {
    Ok(NmbArgumentRun {
        id: r.id,
        count: r.count,
        ids_per_element: r.ids_per_element,
        width: r.width,
        kind: kind_of(r.ty)?,
        nested: r.nested.map_or(0, |n| n as u32 + 1),
        offset: r.offset,
        stride: r.stride,
        element_stride: r.element_stride,
        size: r.size,
    })
}

/// `newArgumentEncoderWithBufferIndex:`'s layouts: the argument buffer `function` takes at buffer `index`, then each one its
/// pointers point to (a run's `nested`: that layout's index + 1), handed to `each` (valid during the call). False with
/// nmb_last_error where Metal makes no encoder, or `each`'s false.
///
/// # Safety
/// `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_argument_layouts(
    function: *mut NmbFunction,
    index: u32,
    each: unsafe extern "C" fn(layouts: *const NmbArgumentLayout, count: u32, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let encoder = get(function)?
            .function()?
            .new_argument_encoder_with_buffer_index(index)
            .map_err(e("argument encoder"))?;
        let flat: Vec<FlatLayout> = flat_layouts(encoder.layout());
        let runs = flat
            .iter()
            .map(|l| l.runs.iter().map(run_of).collect::<Result<Vec<_>, String>>())
            .collect::<Result<Vec<_>, String>>()?;
        let layouts: Vec<NmbArgumentLayout> = flat
            .iter()
            .zip(&runs)
            .map(|(l, r)| NmbArgumentLayout {
                encoded_length: l.encoded_length,
                alignment: l.alignment,
                runs: r.as_ptr(),
                run_count: r.len() as u32,
                reserved: 0,
            })
            .collect();
        // SAFETY: the caller's contract; `layouts` and `runs` live until after the call.
        Ok(unsafe { each(layouts.as_ptr(), layouts.len() as u32, ctx) })
    })
}
