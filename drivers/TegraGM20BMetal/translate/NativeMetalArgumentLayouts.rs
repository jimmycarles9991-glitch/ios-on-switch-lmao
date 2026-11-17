// SPDX-License-Identifier: MIT
//! A function's argument buffer as the raw path's encoder places its arguments (drivers/TegraGM20BMetal/raw/runtime
//! raw_argument_layout.h): m2v's layout of it (metal2vulkan bind/argument_buffer.rs, from the function's AIR) as flat runs
//! of leaf arguments (bind/argument_runs.rs), the layouts of the argument buffers its pointers point to after it.

use metal2vulkan::bind::{ArgumentBufferLayout, ArgumentRun, ArgumentType, flat_layouts};

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

/// `layout`, then each argument buffer its pointers point to (a run's `nested`: that layout's index + 1), as flat runs
/// handed to `each` (valid during the call)
pub fn with_flat_layouts<R>(
    layout: &ArgumentBufferLayout,
    each: impl FnOnce(&[NmbArgumentLayout]) -> R,
) -> Result<R, String> {
    let flat = flat_layouts(layout);
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
    Ok(each(&layouts))
}
