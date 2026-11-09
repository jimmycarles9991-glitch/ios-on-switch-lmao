// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile of a render state (NativeMetalDirect.h): what m2v's translator makes of the state for
//! each of the pack generator's probe draws (drivers/TegraGM20BMetal/raw/pack/probe.mm), with no pipeline state, command
//! buffer or capture. Only the translator's public API is used; the runtime device lends its compile device and its Vulkan
//! device, on which the variants' layouts are made for NVK's stage compile.

use crate::objects::{NmbDevice, NmbRenderPipelineDescriptor, e, get, guard, raw, render_descriptor};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, DrawCall, DrawCommands, ImageSource,
    NeutralResource, SamplerDescriptor, SamplerSource, StageTable, TextureFacts, WriteSource,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::device::{DeviceDescription, VkExtension, VkFeature};
use metal2vulkan::error::Retry;
use metal2vulkan::metal::{
    PixelFormat, SamplerAddressMode, SamplerMinMagFilter, SamplerMipFilter, TextureType, TextureUsage,
};
use metal2vulkan::pipeline::{RenderPipeline, RenderPipelineDescriptor, SlotClass, SlotInterface, SlotRef, Stage};
use std::ffi::{CString, c_char, c_void};
use std::sync::{Arc, Mutex};

const PROBE_SLOTS: u32 = 31;
const PROBE_TAGGED: u32 = 16;
const RUN_BYTES: [u32; 4] = [256, 16, 4096, 256];
const BOUND_RUN: usize = 3;
const MAX_WRITES: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectStage {
    pub spirv: *const u32,
    pub spirv_bytes: usize,
    pub entry: *const c_char,
    pub stage: u32,
    pub flags: u32,
    pub subgroup_size: u32,
    pub specialization: *const avk::SpecializationInfo<'static>,
}

#[repr(u32)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum NmbDirectSource {
    Buffer,
    Bytes,
    Texture,
    Sampler,
    Combined,
    Attachment,
    StorageAttachment,
    Neutral,
    ColorTable,
    Other,
}

pub const NMB_DIRECT_IMMUTABLE: u32 = u32::MAX;
pub const NMB_DIRECT_NEUTRAL: u32 = u32::MAX - 1;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectWrite {
    pub set: u32,
    pub binding: u32,
    pub element: u32,
    pub ty: i32,
    pub source: NmbDirectSource,
    pub stage: u32,
    pub slot: u32,
    pub sampler: u32,
    pub range: u64,
    pub view_type: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectDraw {
    pub set0: u64,
    pub writes: *const NmbDirectWrite,
    pub write_count: u32,
    pub push_mask: u32,
    pub push: [u32; 32],
    pub push_holes: u32,
    pub vertex_slot: u32,
    pub vertex_stride: u32,
    pub variant: *const c_char,
}

#[repr(C)]
pub struct NmbDirectState {
    pub device: u64,
    pub layout: u64,
    pub stage_count: u32,
    pub stages: [NmbDirectStage; 2],
    pub sample_shading: bool,
    pub storage_form: bool,
    pub blended: u32,
    pub draws: [NmbDirectDraw; 4],
}

/// A translation's failure as the caller's text; one the same request may get past (its deadline, the host's memory) starts
/// "retry: ", which the caller does not keep as the state's refusal
fn compile_error(what: &'static str) -> impl FnOnce(metal2vulkan::error::Error) -> String {
    move |error| match error.retry() {
        Retry::Later => format!("retry: {what}: {error:?}"),
        _ => format!("{what}: {error:?}"),
    }
}

/// The compile device of the probe's barrier form: the runtime's enabled description without the pixel interlock, as the
/// pack's barrier probe ran NVK with it hidden (NVK_DEBUG=no_fsi). One per process, made at its first ask.
static BARRIER: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);

fn barrier_device(nmb: &NmbDevice) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = BARRIER.lock().map_err(|_| "barrier device lock")?;
    let id = nmb as *const NmbDevice as usize;
    if let Some((of, device)) = kept.as_ref().filter(|(of, _)| *of == id) {
        let _ = of;
        return Ok(device.clone());
    }
    let runtime = nmb.device();
    // SAFETY: the handles are the live runtime device's, used only for this query.
    let handles = unsafe { runtime.raw() };
    let description =
        unsafe { DeviceDescription::query(handles.instance, handles.physical_device, handles.instance_api_version) }
            .map_err(e("device description"))?;
    let mut enabled = description
        .with_enabled(runtime.enabled())
        .map_err(e("enabled description"))?;
    enabled.generated_commands = None;
    let policy = *runtime.compile_device().policy();
    let same = metal2vulkan::device::Device::new(enabled.clone(), policy).map_err(e("compile device"))?;
    if same.profile_fingerprint() != runtime.compile_device().profile_fingerprint() {
        return Err("the runtime's compile device is not its enabled description's".into());
    }
    enabled.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
    enabled.features.remove(&VkFeature::FragmentShaderPixelInterlock);
    enabled.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    let device = Arc::new(metal2vulkan::device::Device::new(enabled, policy).map_err(e("barrier compile device"))?);
    *kept = Some((id, device.clone()));
    Ok(device)
}

fn stage_number(stage: Stage) -> u32 {
    match stage {
        Stage::Vertex => 1,
        Stage::Fragment => 2,
        _ => 0,
    }
}

/// probe.mm's sampler at fragment slot `s`: linear filters when s&1, a repeating U when s&2, max LOD 1 + s/2
fn probe_sampler(s: u32) -> SamplerDescriptor {
    let mut d = SamplerDescriptor::new();
    let filter = if s & 1 != 0 {
        SamplerMinMagFilter::Linear
    } else {
        SamplerMinMagFilter::Nearest
    };
    d.min_filter = filter;
    d.mag_filter = filter;
    d.mip_filter = SamplerMipFilter::Nearest;
    if s & 2 != 0 {
        d.s_address_mode = SamplerAddressMode::Repeat;
    }
    d.lod_max_clamp = 1.0 + 0.5 * s as f32;
    d
}

/// pack/probe_bind.h probeBindOffset: a uniform slot's residual is the offset itself (16 or more), a storage slot's its low
/// four bits (4, 8 or 12 at the vertex stage, which stays 4-aligned for fixed-function vertex buffers; at the fragment stage
/// one of the twelve that are not multiples of 4)
fn probe_bind_offset(stage: u32, slot: u32) -> u64 {
    const FRAGMENT_CODES: [u64; 12] = [1, 2, 3, 5, 6, 7, 9, 10, 11, 13, 14, 15];
    let slot = u64::from(slot);
    if stage == 2 {
        16 * (12 + slot / 12) + FRAGMENT_CODES[(slot % 12) as usize]
    } else {
        16 * (1 + slot / 3) + 4 * (1 + slot % 3)
    }
}

/// The fragment texture slots the probe binds whose arguments the state's functions declare `texture3d` (Core Animation's
/// colour cubes): a 3D texture bound there gives each its own write, at its own Metal index
fn volume_slots(pipeline: &RenderPipeline) -> Vec<u32> {
    pipeline
        .interface
        .slots
        .iter()
        .filter(|(slot, interface)| {
            slot.stage == Stage::Fragment
                && slot.class == SlotClass::Texture
                && slot.index < PROBE_TAGGED
                && matches!(interface, SlotInterface::Texture(t) if t.texture_type == TextureType::Type3D)
        })
        .map(|(slot, _)| slot.index)
        .collect()
}

/// The bind state of probe.mm's draw `run`: every buffer slot of both stages bound (inline bytes, or the bound run's
/// buffers at each slot's own offset), 16 textures of `format` and `samples` (3D at `volumes`) and 16 samplers at the
/// fragment stage
fn probe_state(run: usize, format: PixelFormat, samples: u32, volumes: &[u32]) -> BindState {
    let mut state = BindState::default();
    for stage in [Stage::Vertex, Stage::Fragment] {
        let mut table = StageTable::default();
        for slot in 0..PROBE_SLOTS {
            let bound = if run == BOUND_RUN {
                let address = 0x1_0000_0000u64 + (u64::from(stage_number(stage)) << 24) + (u64::from(slot) << 16);
                BoundBuffer::Buffer {
                    length: 256 + u64::from(RUN_BYTES[run]),
                    offset: probe_bind_offset(stage_number(stage), slot),
                    gpu_address: address,
                    device_address: Some(address),
                    attribute_stride: None,
                }
            } else {
                BoundBuffer::Bytes {
                    len: RUN_BYTES[run],
                    attribute_stride: None,
                }
            };
            table.buffers.insert(slot, bound);
        }
        if stage == Stage::Fragment {
            for t in 0..PROBE_TAGGED {
                let mut facts = TextureFacts::new(TextureType::Type2D, format);
                if volumes.contains(&t) {
                    facts.texture_type = TextureType::Type3D;
                } else if samples > 1 {
                    facts.texture_type = TextureType::Type2DMultisample;
                    facts.sample_count = samples;
                    facts.usage |= TextureUsage::RENDER_TARGET;
                }
                table.textures.insert(
                    t,
                    BoundTexture::Texture {
                        facts,
                        resource_id: None,
                    },
                );
                table.samplers.insert(t, BoundSampler::new(probe_sampler(t)));
            }
        }
        state.stages.insert(stage, table);
    }
    state
}

const PROBE_DRAW: DrawCall = DrawCall::Primitives {
    vertex_start: 0,
    vertex_count: 4,
    instance_count: 1,
    base_instance: 0,
};

fn slot_parts(slot: &SlotRef) -> (u32, u32) {
    (stage_number(slot.stage), slot.index)
}

fn neutral_view(neutral: &NeutralResource) -> i32 {
    match neutral {
        NeutralResource::Image { view_type, .. } => avk::ImageViewType::from(*view_type).as_raw(),
        _ => -1,
    }
}

fn write_of(w: &metal2vulkan::bind::DescriptorWrite, commands: &DrawCommands) -> NmbDirectWrite {
    let mut out = NmbDirectWrite {
        set: w.set,
        binding: w.binding,
        element: w.array_element,
        ty: avk::DescriptorType::from(w.ty).as_raw(),
        source: NmbDirectSource::Other,
        stage: 0,
        slot: 0,
        sampler: 0,
        range: 0,
        view_type: -1,
    };
    let sampler_of = |s: &SamplerSource| match s {
        SamplerSource::Slot { slot, .. } => slot.index,
        SamplerSource::Immutable => NMB_DIRECT_IMMUTABLE,
        _ => NMB_DIRECT_NEUTRAL,
    };
    match &w.source {
        WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => {
            out.source = if matches!(w.source, WriteSource::Buffer { .. }) {
                NmbDirectSource::Buffer
            } else {
                NmbDirectSource::Bytes
            };
            (out.stage, out.slot) = slot_parts(slot);
            out.range = commands
                .descriptor_ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        WriteSource::Texture { slot, view } => {
            out.source = NmbDirectSource::Texture;
            (out.stage, out.slot) = slot_parts(slot);
            out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
        }
        WriteSource::Sampler { slot, .. } => {
            out.source = NmbDirectSource::Sampler;
            (out.stage, out.slot) = slot_parts(slot);
            out.sampler = slot.index;
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            out.source = NmbDirectSource::Combined;
            match image {
                ImageSource::Texture { slot, view } => {
                    (out.stage, out.slot) = slot_parts(slot);
                    out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
                }
                ImageSource::Neutral(neutral) => {
                    out.slot = NMB_DIRECT_NEUTRAL;
                    out.view_type = neutral_view(neutral);
                }
            }
            out.sampler = sampler_of(sampler);
        }
        WriteSource::Attachment { color_index } => {
            out.source = NmbDirectSource::Attachment;
            out.slot = *color_index;
        }
        WriteSource::TileAttachment { color_index, .. } => {
            out.source = NmbDirectSource::StorageAttachment;
            out.slot = *color_index;
        }
        WriteSource::Neutral(neutral) => {
            out.source = NmbDirectSource::Neutral;
            out.view_type = neutral_view(neutral);
        }
        WriteSource::ColorWriteTable => {
            out.source = NmbDirectSource::ColorTable;
            out.stage = 2;
        }
        _ => {}
    }
    out
}

/// A variant's Vulkan layout on the runtime's device, as the runtime makes it (pipeline/objects.rs create_set_layout)
struct Layout<'d> {
    device: &'d ash::Device,
    samplers: Vec<avk::Sampler>,
    sets: Vec<avk::DescriptorSetLayout>,
    pipeline: avk::PipelineLayout,
}

impl<'d> Layout<'d> {
    fn new(device: &'d ash::Device, pipeline: &RenderPipeline) -> Result<Layout<'d>, String> {
        let mut made = Layout {
            device,
            samplers: Vec::new(),
            sets: Vec::new(),
            pipeline: avk::PipelineLayout::null(),
        };
        for set in &pipeline.layout.sets {
            let mut immutable: Vec<Vec<avk::Sampler>> = Vec::with_capacity(set.bindings.len());
            for binding in &set.bindings {
                let Some(desc) = binding.immutable_sampler else {
                    immutable.push(Vec::new());
                    continue;
                };
                let mut reduction = avk::SamplerReductionModeCreateInfo::default();
                let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
                let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
                // SAFETY: `info` and its chain are live for the call.
                let sampler = unsafe { device.create_sampler(&info, None) }.map_err(e("sampler"))?;
                made.samplers.push(sampler);
                immutable.push(vec![sampler; binding.count as usize]);
            }
            let bindings: Vec<avk::DescriptorSetLayoutBinding<'_>> = set
                .bindings
                .iter()
                .zip(&immutable)
                .map(|(binding, samplers)| {
                    let info = avk::DescriptorSetLayoutBinding::default()
                        .binding(binding.binding)
                        .descriptor_type(binding.ty.into())
                        .descriptor_count(binding.count)
                        .stage_flags(binding.stages.into());
                    if samplers.is_empty() {
                        info
                    } else {
                        info.immutable_samplers(samplers)
                    }
                })
                .collect();
            let flags: Vec<avk::DescriptorBindingFlags> = set.bindings.iter().map(|b| b.flags.into()).collect();
            let mut flags_info = avk::DescriptorSetLayoutBindingFlagsCreateInfo::default().binding_flags(&flags);
            let mut info = avk::DescriptorSetLayoutCreateInfo::default()
                .flags(set.flags.into())
                .bindings(&bindings);
            if flags.iter().any(|f| !f.is_empty()) {
                info = info.push_next(&mut flags_info);
            }
            // SAFETY: `info` and its arrays are live for the call.
            made.sets
                .push(unsafe { device.create_descriptor_set_layout(&info, None) }.map_err(e("set layout"))?);
        }
        let push: Vec<avk::PushConstantRange> = pipeline
            .layout
            .push_constants
            .iter()
            .map(|range| {
                avk::PushConstantRange::default()
                    .stage_flags(range.stages.into())
                    .offset(range.offset)
                    .size(range.size)
            })
            .collect();
        let info = avk::PipelineLayoutCreateInfo::default()
            .set_layouts(&made.sets)
            .push_constant_ranges(&push);
        // SAFETY: as above.
        made.pipeline = unsafe { device.create_pipeline_layout(&info, None) }.map_err(e("pipeline layout"))?;
        Ok(made)
    }

    fn set0(&self) -> u64 {
        self.sets.first().map_or(0, |s| avk::Handle::as_raw(*s))
    }
}

impl Drop for Layout<'_> {
    fn drop(&mut self) {
        // SAFETY: made here, used by no pipeline or set: the stage compile only reads them.
        unsafe {
            self.device.destroy_pipeline_layout(self.pipeline, None);
            for &set in &self.sets {
                self.device.destroy_descriptor_set_layout(set, None);
            }
            for &sampler in &self.samplers {
                self.device.destroy_sampler(sampler, None);
            }
        }
    }
}

/// One probe draw: the variant it runs on (its index among `variants`), its plan and its commands
struct Run {
    variant: usize,
    commands: DrawCommands,
    plan: BindingPlan,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    variants: &mut Vec<(VariantKey, RenderPipeline)>,
    state: &BindState,
    budget: &Budget,
) -> Result<Run, String> {
    let shape = state.shape();
    let plan = variants[0].1.plan(compile, &shape).map_err(e("plan"))?;
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
        (at, variants[at].1.plan(compile, &shape).map_err(e("plan variant"))?)
    };
    let commands = plan.instantiate_draw(state, &PROBE_DRAW).map_err(e("instantiate"))?;
    Ok(Run {
        variant,
        commands,
        plan,
    })
}

fn draw_of(
    run: &Run,
    pipeline: &RenderPipeline,
    layout: &Layout<'_>,
    writes: &[NmbDirectWrite],
    variant: &CString,
) -> NmbDirectDraw {
    let mut draw = NmbDirectDraw {
        set0: layout.set0(),
        writes: writes.as_ptr(),
        write_count: writes.len() as u32,
        push_mask: 0,
        push: [0; 32],
        push_holes: run.commands.push_holes.len() as u32,
        vertex_slot: u32::MAX,
        vertex_stride: 0,
        variant: variant.as_ptr(),
    };
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
                _ => run
                    .commands
                    .vertex_strides
                    .iter()
                    .find(|(at, _)| *at == first.binding)
                    .map_or(0, |(_, s)| *s),
            },
            None => 0,
        };
    }
    draw
}

/// `texture_format`, `texture_samples`: the probe's tagged textures' MTLPixelFormat (0: BGRA8Unorm) and sample count, as
/// probe.mm binds them for the state.
///
/// # Safety
/// `descriptor` is a valid record; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_direct(
    device: *mut NmbDevice,
    descriptor: *const NmbRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let nmb = get(device)?;
        let runtime = nmb.device();
        let descriptor = render_descriptor(descriptor)?;
        let barrier_compile = if barrier { Some(barrier_device(nmb)?) } else { None };
        let compile = barrier_compile.as_deref().unwrap_or(runtime.compile_device());
        let format = match texture_format {
            0 => PixelFormat::BGRA8Unorm,
            f => raw::<PixelFormat>(f, "texture format")?,
        };
        let budget = Budget::standard();
        let base = compile
            .compile_render_pipeline(&descriptor, &budget)
            .map_err(compile_error("compile"))?;
        let volumes = volume_slots(&base);
        let mut variants = vec![(VariantKey::new(), base)];
        let mut runs = Vec::with_capacity(RUN_BYTES.len());
        for run in 0..RUN_BYTES.len() {
            runs.push(settle(
                compile,
                &descriptor,
                &mut variants,
                &probe_state(run, format, texture_samples.max(1), &volumes),
                &budget,
            )?);
        }
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { runtime.raw() };
        let layouts = variants
            .iter()
            .map(|(_, pipeline)| Layout::new(handles.device, pipeline))
            .collect::<Result<Vec<_>, String>>()?;
        let writes: Vec<Vec<NmbDirectWrite>> = runs
            .iter()
            .map(|run| {
                run.plan
                    .descriptors
                    .iter()
                    .take(MAX_WRITES)
                    .map(|w| write_of(w, &run.commands))
                    .collect()
            })
            .collect();
        let names: Vec<CString> = runs
            .iter()
            .map(|run| CString::new(format!("{:?}", run.plan.variant)).unwrap_or_default())
            .collect();
        let draws: Vec<NmbDirectDraw> = runs
            .iter()
            .enumerate()
            .map(|(k, run)| {
                draw_of(
                    run,
                    &variants[run.variant].1,
                    &layouts[run.variant],
                    &writes[k],
                    &names[k],
                )
            })
            .collect();

        let first = &runs[0];
        let pipeline = &variants[first.variant].1;
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
        if pipeline.stages.len() > 2 {
            return Err(format!("{} stages", pipeline.stages.len()));
        }
        let mut state = NmbDirectState {
            device: avk::Handle::as_raw(handles.device.handle()),
            layout: avk::Handle::as_raw(layouts[first.variant].pipeline),
            stage_count: pipeline.stages.len() as u32,
            stages: [NmbDirectStage {
                spirv: std::ptr::null(),
                spirv_bytes: 0,
                entry: std::ptr::null(),
                stage: 0,
                flags: 0,
                subgroup_size: 0,
                specialization: std::ptr::null(),
            }; 2],
            sample_shading: pipeline.graphics.sample_shading,
            storage_form: !barrier && !variants[0].1.graphics.storage_attachments.is_empty(),
            blended: pipeline
                .graphics
                .color_attachments
                .iter()
                .enumerate()
                .filter(|(_, c)| c.as_ref().is_some_and(|c| c.blend.is_some()))
                .fold(0, |mask, (j, _)| mask | 1 << j),
            draws: [draws[0], draws[1], draws[2], draws[3]],
        };
        for (k, stage) in pipeline.stages.iter().enumerate() {
            state.stages[k] = NmbDirectStage {
                spirv: stage.spirv.as_ptr(),
                spirv_bytes: stage.spirv.len() * 4,
                entry: entries[k].as_ptr(),
                stage: avk::ShaderStageFlags::from(stage.stage).as_raw(),
                flags: avk::PipelineShaderStageCreateFlags::from(stage.stage_flags).as_raw(),
                subgroup_size: stage.required_subgroup_size.unwrap_or(0),
                specialization: if maps[k].is_empty() {
                    std::ptr::null()
                } else {
                    (&infos[k] as *const avk::SpecializationInfo<'_>).cast()
                },
            };
        }
        // SAFETY: the caller's contract; everything `state` points at lives until after the call.
        Ok(unsafe { each(&state, ctx) })
    })
}
