// SPDX-License-Identifier: MIT

use crate::descriptors::e;
use crate::direct::compile_error;
use crate::direct_abi::{GUEST_BASE_FRAGMENT, GUEST_BASES, HEAP_ARRAYS, NmbDirectDraw, NmbDirectWrite};
use crate::direct_layout::{Layout, heap_of};
use crate::direct_writes::residual_words;
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{BindState, BindingPlan, DrawCall, DrawCommands, FieldLocation, PushHole};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::error::BindError;
use metal2vulkan::metal::VertexStepFunction;
use metal2vulkan::pipeline::{PushField, RenderPipeline, RenderPipelineDescriptor, SlotRef, Stage, VertexStride};
use std::ffi::CString;

/// The stages of a hole's flags as direct.cpp numbers them: bit 0 Metal's vertex stage (a tessellation state's is
/// its evaluation stage, which reads the vertex stage's buffers), bit 1 fragment
fn hole_stages(stages: metal2vulkan::vk::ShaderStageFlags) -> u32 {
    use metal2vulkan::vk::ShaderStageFlags as S;
    u32::from(stages.intersects(S::VERTEX | S::TESSELLATION_EVALUATION)) | u32::from(stages.contains(S::FRAGMENT)) << 1
}

/// The holes of `commands` the driver fills into `draw` (each stage's root-table and ExecutionStatus address, as push word
/// + 1), the others counted; and the buffer slots each stage's root table holds an entry for, as the bound run names them
fn holes_of(commands: &DrawCommands, draw: &mut NmbDirectDraw) -> Result<(), String> {
    for hole in &commands.push_holes {
        let (word, stages, offset) = match *hole {
            PushHole::RootTableAddress { stages, offset } => (&mut draw.root_table_word, stages, offset),
            PushHole::ExecutionStatusAddress { stages, offset } => (&mut draw.status_word, stages, offset),
            _ => {
                draw.other_holes += 1;
                continue;
            }
        };
        if offset % 4 != 0 || offset > 128 - 8 {
            return Err("an address push hole outside the root push window".into());
        }
        let encoded = (offset / 4 + 1) as u8;
        for k in 0..2 {
            if hole_stages(stages) >> k & 1 == 0 {
                continue;
            }
            if word[k] != 0 && word[k] != encoded {
                return Err("a stage's address push hole at two places".into());
            }
            word[k] = encoded;
        }
    }
    for (stages, table) in &commands.root_tables {
        let field = |root: usize, at: usize| {
            table
                .bytes
                .get(32 * root + at..32 * root + at + 8)
                .and_then(|b| b.try_into().ok())
                .map_or(0, u64::from_le_bytes)
        };
        let first = (field(1, 8) as usize).min(table.bytes.len() / 32);
        for root in 2..first {
            if field(root, 0) == 0 {
                continue;
            }
            let slot = root - 2;
            if slot >= 31 {
                return Err("a root table entry past the buffer slots".into());
            }
            for k in 0..2 {
                if hole_stages(*stages) >> k & 1 != 0 {
                    draw.root_slots[k] |= 1 << slot;
                }
            }
        }
    }
    Ok(())
}

/// One probe draw: the variant it runs on (its index among `variants`), its plan, its commands and its bind state
pub(crate) struct Run {
    pub(crate) variant: usize,
    pub(crate) commands: DrawCommands,
    pub(crate) plan: BindingPlan,
    pub(crate) state: BindState,
}

/// Why a probe draw did not settle: a buffer slot m2v reaches by its device address, or whose Metal address its programs
/// read, was bound without one (the probe binds it a buffer then), or the translator's refusal
pub(crate) enum Unsettled {
    Address(SlotRef),
    Refused(String),
}

impl From<String> for Unsettled {
    fn from(why: String) -> Unsettled {
        Unsettled::Refused(why)
    }
}

/// `plan` as a probe draw settles it: a slot bound with `set*Bytes` whose device or Metal address the plan needs is one the
/// probe binds a buffer at; the runtime gives such a slot its upload copy's address (RenderEncoder::guestBaseWords)
fn planned(plan: Result<BindingPlan, BindError>, what: &'static str) -> Result<BindingPlan, Unsettled> {
    match plan {
        Err(BindError::MissingDeviceAddress { slot } | BindError::MissingGuestAddress { slot }) => {
            Err(Unsettled::Address(slot))
        }
        plan => Ok(plan.map_err(e(what))?),
    }
}

pub(crate) fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    variants: &mut Vec<(VariantKey, RenderPipeline)>,
    state: BindState,
    draw: &DrawCall,
    budget: &Budget,
) -> Result<Run, Unsettled> {
    let shape = state.shape();
    let plan = planned(variants[0].1.plan(compile, &shape), "plan")?;
    let (variant, plan) = if plan.variant == variants[0].0 {
        (0, plan)
    } else {
        let at = match variants.iter().position(|(key, _)| *key == plan.variant) {
            Some(at) => at,
            None => {
                let pipeline = compile
                    .compile_render_variant(descriptor, &plan.variant, budget)
                    .map_err(compile_error("compile variant"))?;
                variants.push((plan.variant.clone(), pipeline));
                variants.len() - 1
            }
        };
        (at, planned(variants[at].1.plan(compile, &shape), "plan variant")?)
    };
    let commands = plan.instantiate_draw(&state, draw).map_err(e("instantiate"))?;
    Ok(Run {
        variant,
        commands,
        plan,
        state,
    })
}

pub(crate) fn draw_of(
    run: &Run,
    pipeline: &RenderPipeline,
    layout: &Layout<'_>,
    writes: &[NmbDirectWrite],
    spills: &[Vec<u32>; 2],
    variant: &CString,
    amplification: u32,
) -> Result<NmbDirectDraw, String> {
    let mut draw = NmbDirectDraw {
        set0: layout.set0(),
        set0_bindings: layout.set0_bindings.as_ptr(),
        set0_binding_count: layout.set0_bindings.len() as u32,
        writes: writes.as_ptr(),
        write_count: writes.len() as u32,
        push_mask: 0,
        push: [0; 32],
        half_blend_color_word: 0,
        depth_test_word: 0,
        stencil_test_word: 0,
        shadow_stencil_word: 0,
        push_holes: run.commands.push_holes.len() as u32,
        root_table_word: [0; 2],
        status_word: [0; 2],
        other_holes: 0,
        root_slots: [0; 2],
        set1: 0,
        heap_arrays: 0,
        heap_class: [0; HEAP_ARRAYS],
        vertex_slot: u32::MAX,
        vertex_stride: 0,
        vertex_stride_dynamic: 0,
        pulled_strides: [[0; 2]; 8],
        amplification_max: amplification,
        amplification_id_word: 0,
        amplification_count_word: 0,
        amplification_layer_word: 0,
        amplification_viewport_word: 0,
        array_length_word: 0,
        viewport_count_word: 0,
        pulled_attributes: pipeline
            .vertex_input
            .pulled
            .iter()
            .filter(|a| a.attribute < 32)
            .fold(0, |mask, a| mask | 1 << a.attribute),
        variant: variant.as_ptr(),
        spill: [spills[0].as_ptr(), spills[1].as_ptr()],
        spill_words: [spills[0].len() as u32, spills[1].len() as u32],
        residual_word: [
            residual_words(&run.plan, Stage::Vertex),
            residual_words(&run.plan, Stage::Fragment),
        ],
        guest_base_word: [0; GUEST_BASES],
        guest_base_slot: [0; GUEST_BASES],
    };
    holes_of(&run.commands, &mut draw)?;
    heap_of(pipeline, layout, &mut draw);
    dynamic_words(run, pipeline, &mut draw)?;
    for field in &run.plan.push.fields {
        let (word, bytes) = match field.field {
            PushField::SlotGuestBase(slot) => {
                guest_base(
                    &mut draw.guest_base_word,
                    &mut draw.guest_base_slot,
                    slot,
                    field.location,
                    field.size,
                )?;
                continue;
            }
            PushField::HalfBlendColor => (&mut draw.half_blend_color_word, 16),
            PushField::ShaderTestsState => (&mut draw.depth_test_word, 16),
            PushField::ShaderStencilState => (&mut draw.stencil_test_word, 16),
            PushField::ShadowStencilState => (&mut draw.shadow_stencil_word, 8),
            PushField::RenderTargetArrayLength => (&mut draw.array_length_word, 4),
            PushField::ViewportCount => (&mut draw.viewport_count_word, 4),
            _ => continue,
        };
        // a field past the window is a word of the fragment stage's argument table: spill_table.h numbers it from 32
        let (offset, first, end) = match field.location {
            FieldLocation::PushWindow { offset } => (offset, 0, 128),
            FieldLocation::ArgumentTable { offset } if field.stage == Stage::Fragment => {
                (offset, 128, 4 * spills[1].len() as u32)
            }
            FieldLocation::ArgumentTable { .. } => {
                return Err("a vertex stage's dynamic state in its argument table".into());
            }
            _ => return Err("dynamic state at a push location the driver does not know".into()),
        };
        if field.size != bytes || offset % 4 != 0 || offset > end.saturating_sub(bytes) {
            return Err("invalid dynamic state push placement".into());
        }
        let encoded = (first + offset) / 4 + 1;
        if *word != 0 && *word != encoded {
            return Err("multiple dynamic state push placements".into());
        }
        *word = encoded;
    }
    for write in &run.commands.push_constants {
        for (k, word) in write.bytes.chunks(4).enumerate() {
            let at = write.offset as usize / 4 + k;
            if at < 32 && word.len() == 4 {
                draw.push[at] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                draw.push_mask |= 1 << at;
            }
        }
    }
    if let Some(first) = run.plan.vertex_buffers.iter().min_by_key(|b| b.binding) {
        draw.vertex_slot = first.buffer_index;
        draw.vertex_stride = match pipeline
            .vertex_input
            .bindings
            .iter()
            .find(|b| b.binding == first.binding)
        {
            Some(b) => match b.stride {
                metal2vulkan::pipeline::VertexStride::Static(stride) => stride,
                _ => {
                    draw.vertex_stride_dynamic = 1;
                    run.commands
                        .vertex_strides
                        .iter()
                        .find(|(at, _)| *at == first.binding)
                        .map_or(0, |(_, s)| *s)
                }
            },
            None => 0,
        };
    }
    Ok(draw)
}

/// A SlotGuestBase field as the record keeps it (PackPipeline::guestBaseWord, guestBaseSlot): its push word + 1, and the
/// Metal buffer index whose bound address each draw writes there, bit 7 set for the fragment stage's
fn guest_base(
    words: &mut [u8; GUEST_BASES],
    slots: &mut [u8; GUEST_BASES],
    slot: SlotRef,
    location: FieldLocation,
    size: u32,
) -> Result<(), String> {
    let FieldLocation::PushWindow { offset } = location else {
        return Err("a buffer's Metal address placed past the root push window".into());
    };
    if size != 8 || offset % 4 != 0 || offset > 128 - 8 {
        return Err("invalid Metal address push placement".into());
    }
    let stage = match slot.stage {
        Stage::Vertex => 0,
        Stage::Fragment => GUEST_BASE_FRAGMENT,
        _ => return Err(format!("a Metal address of a {:?} stage's buffer", slot.stage)),
    };
    if slot.index >= 31 {
        return Err("a Metal address of a slot past the buffer slots".into());
    }
    let (word, bound) = ((offset / 4 + 1) as u8, stage | slot.index as u8);
    if let Some(at) = words.iter().position(|&w| w == word) {
        return if slots[at] == bound {
            Ok(())
        } else {
            Err("two buffers' Metal addresses at one push word".into())
        };
    }
    let at = words
        .iter()
        .position(|&w| w == 0)
        .ok_or_else(|| format!("more than {GUEST_BASES} buffers' Metal addresses read"))?;
    words[at] = word;
    slots[at] = bound;
    Ok(())
}

/// The push words a raw draw writes itself: each amplification's id, count and offsets, and each dynamic pulled layout's
/// stride
fn dynamic_words(run: &Run, pipeline: &RenderPipeline, draw: &mut NmbDirectDraw) -> Result<(), String> {
    let mut pulled = 0;
    for field in &run.plan.push.fields {
        let FieldLocation::PushWindow { offset } = field.location else {
            continue;
        };
        let word = offset / 4 + 1;
        match field.field {
            PushField::AmplificationId => draw.amplification_id_word = word,
            PushField::AmplificationCount => draw.amplification_count_word = word,
            PushField::AmplificationLayerOffset => draw.amplification_layer_word = word,
            PushField::AmplificationViewportOffset => draw.amplification_viewport_word = word,
            PushField::VertexBufferStride(layout)
                if pipeline
                    .vertex_input
                    .pulled
                    .iter()
                    .any(|a| a.buffer_index == layout && a.stride == VertexStride::PerBind) =>
            {
                if pulled == draw.pulled_strides.len() {
                    return Err("more than eight pulled vertex buffers of a dynamic stride".into());
                }
                draw.pulled_strides[pulled] = [layout, word];
                pulled += 1;
            }
            _ => {}
        }
    }
    let dynamic: std::collections::BTreeSet<u32> = pipeline
        .vertex_input
        .pulled
        .iter()
        .filter(|a| a.stride == VertexStride::PerBind && a.step_function != VertexStepFunction::Constant)
        .map(|a| a.buffer_index)
        .collect();
    if pulled != dynamic.len() {
        return Err("a pulled vertex buffer of a dynamic stride whose stride word is past the root push window".into());
    }
    Ok(())
}

/// The pipeline's stages in NmbDirectState's order: vertex and fragment, then a tessellation state's control and
/// evaluation; a state of no fragment function (rasterization on or off) its vertex stage alone
pub(crate) fn stage_order(pipeline: &RenderPipeline) -> Result<Vec<usize>, String> {
    let wanted: &[avk::ShaderStageFlags] = if pipeline.graphics.tessellation.is_some() {
        &[
            avk::ShaderStageFlags::VERTEX,
            avk::ShaderStageFlags::FRAGMENT,
            avk::ShaderStageFlags::TESSELLATION_CONTROL,
            avk::ShaderStageFlags::TESSELLATION_EVALUATION,
        ]
    } else if pipeline.stages.len() == 1 {
        &[avk::ShaderStageFlags::VERTEX]
    } else {
        &[avk::ShaderStageFlags::VERTEX, avk::ShaderStageFlags::FRAGMENT]
    };
    if pipeline.stages.len() != wanted.len() {
        return Err(format!("{} stages", pipeline.stages.len()));
    }
    wanted
        .iter()
        .map(|&flag| {
            pipeline
                .stages
                .iter()
                .position(|s| avk::ShaderStageFlags::from(s.stage) == flag)
                .ok_or_else(|| format!("no stage of flags {:#x}", flag.as_raw()))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::{GUEST_BASE_FRAGMENT, GUEST_BASES, guest_base, hole_stages};
    use metal2vulkan::bind::FieldLocation;
    use metal2vulkan::pipeline::{SlotClass, SlotRef, Stage};
    use metal2vulkan::vk::ShaderStageFlags as S;

    fn buffer(stage: Stage, index: u32) -> SlotRef {
        SlotRef {
            stage,
            class: SlotClass::Buffer,
            index,
        }
    }

    #[test]
    fn a_buffers_metal_address_is_kept_as_its_push_word_and_slot() {
        let (mut words, mut slots) = ([0; GUEST_BASES], [0; GUEST_BASES]);
        let at = |offset| FieldLocation::PushWindow { offset };
        guest_base(&mut words, &mut slots, buffer(Stage::Vertex, 5), at(24), 8).unwrap();
        guest_base(&mut words, &mut slots, buffer(Stage::Fragment, 2), at(120), 8).unwrap();
        guest_base(&mut words, &mut slots, buffer(Stage::Vertex, 5), at(24), 8).unwrap();
        assert_eq!(words, [7, 31, 0, 0]);
        assert_eq!(slots, [5, GUEST_BASE_FRAGMENT | 2, 0, 0]);
    }

    #[test]
    fn a_metal_address_the_runtime_cannot_write_is_refused() {
        let (mut words, mut slots) = ([0; GUEST_BASES], [0; GUEST_BASES]);
        let at = |offset| FieldLocation::PushWindow { offset };
        let vertex = buffer(Stage::Vertex, 1);
        assert!(
            guest_base(
                &mut words,
                &mut slots,
                vertex,
                FieldLocation::ArgumentTable { offset: 0 },
                8
            )
            .is_err()
        );
        assert!(guest_base(&mut words, &mut slots, vertex, at(124), 8).is_err());
        assert!(guest_base(&mut words, &mut slots, vertex, at(6), 8).is_err());
        assert!(guest_base(&mut words, &mut slots, vertex, at(8), 4).is_err());
        assert!(guest_base(&mut words, &mut slots, buffer(Stage::Kernel, 1), at(8), 8).is_err());
        assert!(guest_base(&mut words, &mut slots, buffer(Stage::Vertex, 31), at(8), 8).is_err());
        assert_eq!(words, [0; GUEST_BASES]);
        guest_base(&mut words, &mut slots, vertex, at(8), 8).unwrap();
        assert!(guest_base(&mut words, &mut slots, buffer(Stage::Vertex, 2), at(8), 8).is_err());
        for (k, offset) in [16, 32, 40].into_iter().enumerate() {
            guest_base(
                &mut words,
                &mut slots,
                buffer(Stage::Vertex, 2 + k as u32),
                at(offset),
                8,
            )
            .unwrap();
        }
        assert!(guest_base(&mut words, &mut slots, buffer(Stage::Vertex, 9), at(56), 8).is_err());
    }

    #[test]
    fn a_tessellation_states_evaluation_holes_are_its_vertex_stages() {
        assert_eq!(hole_stages(S::TESSELLATION_EVALUATION), 1);
        assert_eq!(hole_stages(S::TESSELLATION_EVALUATION | S::FRAGMENT), 3);
        assert_eq!(hole_stages(S::VERTEX), 1);
        assert_eq!(hole_stages(S::FRAGMENT), 2);
        assert_eq!(hole_stages(S::TESSELLATION_CONTROL), 0);
    }
}
