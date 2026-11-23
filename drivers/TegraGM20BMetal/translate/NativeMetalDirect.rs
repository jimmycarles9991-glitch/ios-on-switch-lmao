// SPDX-License-Identifier: MIT
//! The driver's direct compile of a render state (NativeMetalDirect.h): what m2v's translator makes of the
//! state for each of the pack generator's probe draws (drivers/TegraGM20BMetal/pack/probe.mm), with no pipeline
//! state, command buffer or capture. Only the translator's public API is used. A runtime device lends its compile
//! device and its Vulkan device, on which the variants' layouts are made too; the offline compile (the pack
//! generator's, and the system shader compiler's compile context: NativeMetalCompile.rs) runs on the compile profile's
//! devices with the layouts as data alone.

use crate::descriptors::{DescriptorFunction, NmbRenderPipelineDescriptorOf, guard, raw, render_descriptor};
use crate::direct_abi::{
    LAYER_DROPPED, NmbDirectBinds, NmbDirectDraw, NmbDirectSource, NmbDirectStage, NmbDirectState, NmbDirectStorage,
    NmbDirectTess, NmbDirectWrite,
};
use crate::direct_draw::{Run, Unsettled, draw_of, settle, stage_order};
use crate::direct_layout::Layout;
use crate::direct_probe::{Binds, RUN_BYTES, declared_slots, probe_state, probed_format, vertex_samplers};
use crate::direct_writes::{immutable_samplers, spill_table, table_of, write_of};
use crate::offline::{Devices, NmbOfflineRenderPipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::cache::VariantKey;
use metal2vulkan::error::Retry;
use metal2vulkan::metal::PixelFormat;
use metal2vulkan::pipeline::{
    GraphicsState, RenderPipeline, RenderPipelineDescriptor, RuntimeFact, SlotRef, Stage, StageSource, StandInView,
    StorageAttachmentView,
};
use std::ffi::{CString, c_void};

/// A render stage's Metal arguments on this identity (set_entries.h): 31 buffers, the A10X's 31 textures (21H461) or the
/// A13's 128 (24B119, `target_j181`), 16 samplers
#[cfg(not(target_j181))]
const STAGE_TEXTURES: usize = 31;
#[cfg(target_j181)]
const STAGE_TEXTURES: usize = 128;
const STAGE_ARGUMENTS: usize = 31 + STAGE_TEXTURES + 16;
/// set_entries.h kSyntheticEntries: the colour table, the handle and argument tables, the tessellation buffers, the
/// depth and stencil stand-ins, and each colour's fetch
const SYNTHETIC_WRITES: usize = 1 + 4 + 2 + 3 + 2 + 8;
/// The set-0 writes a state's record holds at most: every argument one render pipeline binds, and every synthetic entry
const MAX_WRITES: usize = 2 * STAGE_ARGUMENTS + SYNTHETIC_WRITES;

fn storage_of(graphics: &GraphicsState) -> Result<NmbDirectStorage, String> {
    let mut storage = NmbDirectStorage::default();
    if let Some(tests) = graphics.shader_tests {
        storage.depth_format = tests.depth.format.as_raw() as u32;
        storage.test_samples = tests.depth.samples;
        storage.test_planes = u32::from(!matches!(tests.depth.view, StandInView::Image));
        if let Some(stencil) = tests.stencil {
            if stencil.samples != tests.depth.samples {
                return Err("storage form: depth and stencil sample counts differ".into());
            }
            storage.stencil_format = stencil.format.as_raw() as u32;
            storage.test_planes |= u32::from(!matches!(stencil.view, StandInView::Image)) << 1;
        }
    }
    if let Some(shadow) = graphics.shadow_stencil {
        storage.shadow_format = shadow.format.as_raw() as u32;
        storage.shadow_samples = shadow.samples;
    }
    for attachment in &graphics.storage_attachments {
        let index = attachment.color_index as usize;
        if index >= storage.formats.len() || storage.mask & (1 << index) != 0 {
            return Err("storage form: invalid color attachment index".into());
        }
        storage.mask |= 1 << index;
        // A write the pipeline's mask leaves no channel of stores nothing: the colour is only read, so its draws need
        // no more than a barrier (describeReads' rung, storage_form.h fetchesOnlyUnwritten)
        storage.writes |= u32::from(attachment.stores()) << index;
        storage.sample_planes |=
            u32::from(matches!(attachment.view, StorageAttachmentView::SamplePlanes { .. })) << index;
        storage.samples = graphics.rasterization_samples.as_raw();
        storage.formats[index] = attachment.uint_view.unwrap_or(attachment.format).as_raw() as u32;
        storage.blend_mask |= u32::from(attachment.blend_model != 0) << index;
        storage.blend_model |= u32::from(attachment.blend_model);
    }
    Ok(storage)
}

/// A translation's failure as the caller's text; one the same request may get past (its deadline, the host's memory) starts
/// "retry: ", which the caller does not keep as the state's refusal
pub fn compile_error(what: &'static str) -> impl FnOnce(metal2vulkan::error::Error) -> String {
    move |error| match error.retry() {
        Retry::Later => format!("retry: {what}: {error:?}"),
        _ => format!("{what}: {error:?}"),
    }
}

/// A state of more set-0 writes than its record holds is refused, naming it: no write is dropped
fn set0_writes_fit(count: usize, name: impl FnOnce() -> String) -> Result<(), String> {
    if count <= MAX_WRITES {
        return Ok(());
    }
    Err(format!(
        "{}: {count} set-0 descriptor writes, past the {MAX_WRITES} of every argument a render pipeline binds",
        name()
    ))
}

fn state_name(descriptor: &RenderPipelineDescriptor) -> String {
    let name = |stage: &StageSource| match stage {
        StageSource::Function(f) => f.function.info().name.to_owned(),
        StageSource::Compiled(_) => "a compiled stage".to_owned(),
    };
    let fragment = descriptor
        .fragment
        .as_ref()
        .map_or_else(|| "no fragment function".to_owned(), name);
    format!("{} + {fragment}", name(&descriptor.vertex))
}

fn chooses_layer(pipeline: &RenderPipeline) -> bool {
    pipeline
        .dependencies
        .iter()
        .any(|d| matches!(d.fact, RuntimeFact::RenderTargetArrayLength { .. }))
}

/// nmb_render_pipeline_direct's answer for a state of offline functions, made on the compile profile's devices with no
/// Vulkan object: every handle the state hands over is 0.
///
/// # Safety
/// As nmb_render_pipeline_direct's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_offline(
    descriptor: *const NmbOfflineRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        // SAFETY: the caller's contract.
        unsafe {
            render_offline(
                offline_devices()?,
                descriptor,
                barrier,
                texture_format,
                texture_samples,
                binds,
                each,
                ctx,
            )
        }
    })
}

/// nmb_render_pipeline_offline's compile on `devices`, of a descriptor of either function handle: the pack generator's
/// offline functions, or the compile context's (NativeMetalCompile.rs)
///
/// # Safety
/// As nmb_render_pipeline_direct's.
#[allow(clippy::too_many_arguments)]
pub unsafe fn render_offline<F: DescriptorFunction>(
    devices: &Devices,
    descriptor: *const NmbRenderPipelineDescriptorOf<F>,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    // SAFETY: the caller's contract.
    let binds = unsafe { Binds::of(binds) }?;
    let descriptor = render_descriptor(descriptor)?;
    let compile = devices.render(barrier, binds.fragment_tests);
    translate(
        compile,
        &descriptor,
        None,
        texture_format,
        texture_samples,
        &binds,
        each,
        ctx,
    )
}

/// The state `descriptor` describes as the translator makes it on `compile` for the probe's draws, its layouts made on
/// `vulkan` where there is one, handed to `each`
#[allow(clippy::too_many_arguments)]
pub fn translate(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    vulkan: Option<&ash::Device>,
    texture_format: u64,
    texture_samples: u32,
    binds: &Binds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let format = match texture_format {
        0 => PixelFormat::BGRA8Unorm,
        f => raw::<PixelFormat>(f, "texture format")?,
    };
    let amplification = descriptor.max_vertex_amplification_count.max(1);
    let budget = crate::compile_budget();
    let base = compile
        .compile_render_pipeline(descriptor, &budget)
        .map_err(compile_error("compile"))?;
    let declared = declared_slots(&base);
    let samplers = vertex_samplers(&base);
    let draw = crate::direct_tess::probe_draw(base.graphics.tessellation.as_ref());
    let patches = base.graphics.tessellation.is_some();
    let chooses = chooses_layer(&base);
    let mut variants = vec![(VariantKey::new(), base)];
    let probe = |variants: &mut Vec<(VariantKey, RenderPipeline)>, layers: u32| -> Result<Vec<Run>, String> {
        let mut addressed: Vec<SlotRef> = Vec::new();
        'settled: loop {
            let mut runs = Vec::with_capacity(RUN_BYTES.len());
            for run in 0..RUN_BYTES.len() {
                let state = probe_state(
                    run,
                    format,
                    texture_samples.max(1),
                    &declared,
                    &samplers,
                    binds,
                    &addressed,
                    patches,
                    amplification,
                    layers,
                );
                match settle(compile, descriptor, variants, state, &draw, &budget) {
                    Ok(settled) => runs.push(settled),
                    Err(Unsettled::Address(slot)) if !addressed.contains(&slot) => {
                        addressed.push(slot);
                        continue 'settled;
                    }
                    Err(Unsettled::Address(slot)) => return Err(format!("plan: no device address at {slot:?}")),
                    Err(Unsettled::Refused(why)) => return Err(why),
                }
            }
            break Ok(runs);
        }
    };
    // A vertex function that chooses its render target slice, or one amplified, is compiled as for a pass of two
    // layers: the variant that writes Layer, reading the pass's length from its push word, draws in a pass of any
    // length. One the translation has no layered form of (a fragment function of tile memory, an imageblock, or the
    // shader-side stencil or depth test) is compiled for one layer, its record marked so (LAYER_DROPPED): a layered
    // pass draws it elsewhere.
    let (runs, layer_dropped) = if amplification > 1 {
        (probe(&mut variants, 2)?, false)
    } else if chooses {
        match probe(&mut variants, 2) {
            Ok(runs) => (runs, false),
            Err(why) => {
                eprintln!(
                    "direct compile: {} has no layered form, made for one layer: {why}",
                    state_name(descriptor)
                );
                variants.truncate(1);
                (probe(&mut variants, 0)?, true)
            }
        }
    } else {
        (probe(&mut variants, 0)?, false)
    };
    let layouts = variants
        .iter()
        .map(|(_, pipeline)| Layout::new(vulkan, &pipeline.layout))
        .collect::<Result<Vec<_>, String>>()?;
    // The layered storage variant's attachments are the runtime's views of the pass's layers (render_target.rs
    // attachment_view_in), which its storage images address at each primitive's layer, and so are its depth and stencil
    // stand-ins (lower/fragment/depth_test.rs)
    let writes: Vec<Vec<NmbDirectWrite>> = runs
        .iter()
        .map(|run| {
            let layout = &variants[run.variant].1.layout;
            let mut writes: Vec<NmbDirectWrite> = run
                .plan
                .descriptors
                .iter()
                .filter(|w| w.set == 0)
                .map(|w| {
                    let mut out = table_of(write_of(w, &run.commands.descriptor_ranges), layout);
                    if run.plan.variant.layered_storage
                        && matches!(
                            out.source,
                            NmbDirectSource::StorageAttachment
                                | NmbDirectSource::DepthStandIn
                                | NmbDirectSource::StencilStandIn
                        )
                    {
                        out.view_type = avk::ImageViewType::TYPE_2D_ARRAY.as_raw();
                    }
                    out.format = probed_format(&run.state, &out);
                    out
                })
                .collect();
            writes.extend(immutable_samplers(layout, &writes));
            set0_writes_fit(writes.len(), || state_name(descriptor))?;
            Ok(writes)
        })
        .collect::<Result<_, String>>()?;
    let spills: Vec<[Vec<u32>; 2]> = runs
        .iter()
        .map(|run| {
            [Stage::Vertex, Stage::Fragment].map(|stage| spill_table(&run.plan, &run.commands.descriptor_bytes, stage))
        })
        .collect();
    let names: Vec<CString> = runs
        .iter()
        .map(|run| CString::new(format!("{:?}", run.plan.variant)).unwrap_or_default())
        .collect();
    let mut draws: Vec<NmbDirectDraw> = runs
        .iter()
        .enumerate()
        .map(|(k, run)| {
            draw_of(
                run,
                &variants[run.variant].1,
                &layouts[run.variant],
                &writes[k],
                &spills[k],
                &names[k],
                amplification,
            )
        })
        .collect::<Result<Vec<_>, String>>()?;
    if layer_dropped {
        for d in &mut draws {
            d.array_length_word = LAYER_DROPPED;
        }
    }

    let first = &runs[0];
    let pipeline = &variants[first.variant].1;
    let mut storage = storage_of(&pipeline.graphics)?;
    for run in &runs {
        if storage_of(&variants[run.variant].1.graphics)? != storage {
            return Err("storage form: probe variants change the attachment contract".into());
        }
    }
    if storage.mask != 0 || storage.depth_format != 0 {
        storage.fragment_tests = binds.fragment_tests;
    }
    let specialization = &first.plan.specialization;
    let entries: Vec<CString> = pipeline
        .stages
        .iter()
        .map(|s| CString::new(s.entry_point.as_str()).unwrap_or_default())
        .collect();
    let maps: Vec<Vec<avk::SpecializationMapEntry>> = pipeline
        .stages
        .iter()
        .map(|stage| {
            specialization
                .entries
                .iter()
                .filter(|entry| stage.specialization.constants.iter().any(|c| c.id == entry.constant_id))
                .map(|&entry| entry.into())
                .collect()
        })
        .collect();
    let infos: Vec<avk::SpecializationInfo<'_>> = maps
        .iter()
        .map(|map| {
            avk::SpecializationInfo::default()
                .map_entries(map)
                .data(&specialization.data)
        })
        .collect();
    let order = stage_order(pipeline)?;
    let tess = pipeline.graphics.tessellation.as_ref();
    let modes = match tess {
        Some(_) => crate::tess_modes::control_modes(&pipeline.stages[order[2]].spirv),
        None => Vec::new(),
    };
    let evaluation = match tess {
        Some(_) => Some(
            crate::tess_modes::evaluation_with_modes(&pipeline.stages[order[3]].spirv, &modes)
                .ok_or("a tessellation state's evaluation stage without its entry point")?,
        ),
        None => None,
    };
    let mut state = NmbDirectState {
        device: vulkan.map_or(0, |d| avk::Handle::as_raw(d.handle())),
        layout: avk::Handle::as_raw(layouts[first.variant].pipeline),
        layout_data: &layouts[first.variant].data,
        stage_count: order.len() as u32,
        stages: [NmbDirectStage {
            spirv: std::ptr::null(),
            spirv_bytes: 0,
            entry: std::ptr::null(),
            stage: 0,
            flags: 0,
            subgroup_size: 0,
            specialization: std::ptr::null(),
            semantic: std::ptr::null(),
            portable_context: std::ptr::null(),
            portable: None,
        }; 4],
        tess: tess.map_or(NmbDirectTess::default(), |t| NmbDirectTess {
            parameters: crate::tess_modes::parameters(&modes),
            control_word: t.push_offset / 4 + 1,
            control_points: t.control_points,
            index_bytes: crate::direct_tess::index_bytes(t),
        }),
        sample_shading: pipeline.graphics.sample_shading,
        storage_form: storage.mask != 0,
        storage,
        blended: pipeline
            .graphics
            .color_attachments
            .iter()
            .enumerate()
            .filter(|(_, c)| c.as_ref().is_some_and(|c| c.blend.is_some()))
            .fold(0, |mask, (j, _)| mask | 1 << j),
        draws: [draws[0], draws[1], draws[2], draws[3]],
    };
    for (slot, &k) in order.iter().enumerate() {
        let stage = &pipeline.stages[k];
        let spirv: &[u32] = match &evaluation {
            Some(words) if slot == 3 => words,
            _ => &stage.spirv,
        };
        state.stages[slot] = NmbDirectStage {
            spirv: spirv.as_ptr(),
            spirv_bytes: spirv.len() * 4,
            entry: entries[k].as_ptr(),
            stage: avk::ShaderStageFlags::from(stage.stage).as_raw(),
            flags: avk::PipelineShaderStageCreateFlags::from(stage.stage_flags).as_raw(),
            subgroup_size: stage.required_subgroup_size.unwrap_or(0),
            specialization: if maps[k].is_empty() {
                std::ptr::null()
            } else {
                (&infos[k] as *const avk::SpecializationInfo<'_>).cast()
            },
            semantic: std::ptr::null(),
            portable_context: std::ptr::null(),
            portable: None,
        };
    }
    // SAFETY: the caller's contract; everything `state` points at lives until after the call.
    Ok(unsafe { each(&state, ctx) })
}

#[cfg(test)]
mod tests {
    use super::{MAX_WRITES, STAGE_TEXTURES, set0_writes_fit};

    #[test]
    fn a_states_writes_reach_every_argument_of_its_identity_and_its_synthetic_entries() {
        assert_eq!(MAX_WRITES, 2 * (31 + STAGE_TEXTURES + 16) + 20);
        #[cfg(not(target_j181))]
        assert_eq!(MAX_WRITES, 156 + 20);
        #[cfg(target_j181)]
        assert_eq!(MAX_WRITES, 350 + 20);
    }

    #[test]
    fn a_state_past_them_is_refused_by_name_not_truncated() {
        assert!(set0_writes_fit(MAX_WRITES, || "v + f".to_owned()).is_ok());
        let why = set0_writes_fit(MAX_WRITES + 1, || "v + f".to_owned()).unwrap_err();
        assert!(why.starts_with("v + f: "), "{why}");
        assert!(
            why.contains(&format!("{} set-0 descriptor writes", MAX_WRITES + 1)),
            "{why}"
        );
    }
}
