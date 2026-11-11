// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile of a compute state at one dispatch shape (NativeMetalDirect.h): what m2v's translator
//! makes of the kernel for the three dispatches the pack's kernel probe ran (drivers/TegraGM20BMetal/raw/pack/kernels.py),
//! with no pipeline state, command buffer or capture. Only the translator's public API is used; the runtime device lends its
//! compile device and its Vulkan device, on which the variant's layout and samplers are made for NVK.

use crate::direct::{Layout, NMB_DIRECT_IMMUTABLE, NmbDirectStage, NmbDirectWrite, compile_error, write_of};
use crate::objects::{
    NmbComputePipelineDescriptor, NmbDevice, NmbSamplerDescriptor, compute_descriptor, e, get, guard, raw,
    sampler_descriptor,
};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, Dispatch, DispatchCommands, FieldLocation,
    ImageSource, SamplerSource, StageTable, TextureFacts, WriteSource,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::metal::{PixelFormat, TextureSwizzleChannels, TextureType, TextureUsage};
use metal2vulkan::pipeline::{ComputePipeline, ComputePipelineDescriptor, PushField, Stage, StageSource};
use std::ffi::{CString, c_void};

const SLOTS: u32 = 31;
const TAG_BYTES: u32 = 4096;
const MOVED_OFFSET: u64 = 256;
const GRID: [u32; 3] = [3, 5, 2];

#[repr(C)]
pub struct NmbDirectBoundTexture {
    pub slot: u32,
    pub pixel_format: u64,
    pub texture_type: u64,
    pub usage: u64,
    pub levels: u64,
    pub array_length: u64,
    pub samples: u64,
}

#[repr(C)]
pub struct NmbDirectBoundSampler {
    pub slot: u32,
    pub descriptor: NmbSamplerDescriptor,
}

#[repr(C)]
pub struct NmbDirectDispatch {
    pub threads: [u32; 3],
    pub memory: [u32; 31],
    pub textures: *const NmbDirectBoundTexture,
    pub texture_count: u32,
    pub samplers: *const NmbDirectBoundSampler,
    pub sampler_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectKernelWrite {
    pub write: NmbDirectWrite,
    pub view_format: u32,
    pub components: [u32; 4],
    pub sampler: u64,
}

#[repr(C)]
pub struct NmbDirectKernel {
    pub device: u64,
    pub layout: u64,
    pub set0: u64,
    pub stage: NmbDirectStage,
    pub writes: *const NmbDirectKernelWrite,
    pub write_count: u32,
    pub push_mask: u32,
    pub push: [u32; 32],
    pub moved_mask: u32,
    pub moved: [u32; 32],
    pub bound_mask: u32,
    pub bound: [u32; 32],
    pub moved_same: bool,
    pub bound_same: bool,
    pub region_word: i32,
    pub regions: u32,
    pub push_holes: u32,
    pub variant: *const std::ffi::c_char,
}

/// Which of the probe's dispatches a bind state is: 4096 inline bytes at every slot, the same bytes `MOVED_OFFSET` into a
/// buffer of each slot's own, or each of those bound `1 + slot % 15` bytes further (pack/probe_bind.h probeComputeCode)
#[derive(Clone, Copy, PartialEq, Eq)]
enum Run {
    Bytes,
    Moved,
    Bound,
}

fn counted<'a, T>(items: *const T, count: u32) -> &'a [T] {
    if items.is_null() || count == 0 {
        return &[];
    }
    // SAFETY: the caller's counted array, live for the call.
    unsafe { std::slice::from_raw_parts(items, count as usize) }
}

fn facts_of(t: &NmbDirectBoundTexture) -> Result<TextureFacts, String> {
    let mut facts = TextureFacts::new(
        raw::<TextureType>(t.texture_type, "texture type")?,
        raw::<PixelFormat>(t.pixel_format, "pixel format")?,
    );
    facts.usage = raw::<TextureUsage>(t.usage, "texture usage")?;
    facts.sample_count = u32::try_from(t.samples.max(1)).map_err(e("sample count"))?;
    facts.array_length = u32::try_from(t.array_length.max(1)).map_err(e("array length"))?;
    facts.mipmap_level_count = u32::try_from(t.levels.max(1)).map_err(e("levels"))?;
    Ok(facts)
}

fn bind_state(run: Run, dispatch: &NmbDirectDispatch) -> Result<BindState, String> {
    let mut table = StageTable::default();
    for slot in 0..SLOTS {
        let bound = if run == Run::Bytes {
            BoundBuffer::Bytes {
                len: TAG_BYTES,
                attribute_stride: None,
            }
        } else {
            let address = 0x1_0000_0000u64 + (u64::from(slot) << 16);
            let code = if run == Run::Bound { 1 + u64::from(slot % 15) } else { 0 };
            BoundBuffer::Buffer {
                length: MOVED_OFFSET + u64::from(TAG_BYTES),
                offset: MOVED_OFFSET + code,
                gpu_address: address,
                device_address: Some(address),
                attribute_stride: None,
            }
        };
        table.buffers.insert(slot, bound);
    }
    for t in counted(dispatch.textures, dispatch.texture_count) {
        table.textures.insert(
            t.slot,
            BoundTexture::Texture {
                facts: facts_of(t)?,
                resource_id: None,
            },
        );
    }
    for s in counted(dispatch.samplers, dispatch.sampler_count) {
        table
            .samplers
            .insert(s.slot, BoundSampler::new(sampler_descriptor(&s.descriptor)?));
    }
    for (index, &length) in dispatch.memory.iter().enumerate() {
        if length != 0 {
            table.threadgroup_lengths.insert(index as u32, length);
        }
    }
    let mut state = BindState::default();
    state.stages.insert(Stage::Kernel, table);
    Ok(state)
}

/// The kernel compiled under variant `key`, as the runtime compiles a state's variant: the function under the key's lowering
/// inputs with the descriptor's stage input and call stack depth, then the pipeline of it
fn compiled(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    key: &VariantKey,
    budget: &Budget,
) -> Result<ComputePipeline, String> {
    let StageSource::Function(function) = &descriptor.kernel else {
        return Err("a kernel compiled already".into());
    };
    let mut inputs = key.lowering_inputs(Stage::Kernel);
    inputs.stage_input = descriptor.stage_input.clone();
    inputs.max_call_stack_depth = Some(descriptor.max_call_stack_depth).filter(|&k| k > 1);
    let function = compile
        .compile_function(function, &inputs, budget)
        .map_err(compile_error("compile kernel"))?;
    let mut d = descriptor.clone();
    d.kernel = StageSource::Compiled(function);
    compile
        .compile_compute_pipeline(&d, budget)
        .map_err(compile_error("compile"))
}

struct Settled {
    pipeline: ComputePipeline,
    plan: BindingPlan,
    commands: DispatchCommands,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    state: &BindState,
    dispatch: &Dispatch,
    budget: &Budget,
) -> Result<Settled, String> {
    let shape = state.shape();
    let base = compiled(compile, descriptor, &VariantKey::new(), budget)?;
    let plan = base.plan(compile, &shape).map_err(e("plan"))?;
    let wanted = plan.dispatch_variant(dispatch).map_err(e("dispatch variant"))?;
    let (pipeline, plan) = if wanted == VariantKey::new() {
        (base, plan)
    } else {
        let pipeline = compiled(compile, descriptor, &wanted, budget)?;
        let plan = pipeline.plan(compile, &shape).map_err(e("plan variant"))?;
        if plan.dispatch_variant(dispatch).map_err(e("dispatch variant"))? != wanted {
            return Err("the plan of a compiled variant asks for another variant".into());
        }
        (pipeline, plan)
    };
    let commands = plan
        .instantiate_dispatch(state, dispatch)
        .map_err(e("instantiate dispatch"))?;
    Ok(Settled {
        pipeline,
        plan,
        commands,
    })
}

fn push_words(commands: &DispatchCommands) -> (u32, [u32; 32]) {
    let (mut mask, mut words) = (0u32, [0u32; 32]);
    let Some(region) = commands.regions.first() else {
        return (mask, words);
    };
    for write in &region.push_constants {
        for (k, word) in write.bytes.chunks(4).enumerate() {
            let at = write.offset as usize / 4 + k;
            if at < 32 && word.len() == 4 {
                words[at] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                mask |= 1 << at;
            }
        }
    }
    (mask, words)
}

/// What a write is in the record: its binding, kind, source and slot (not the range, which the offsets move)
fn layout_of(plan: &BindingPlan) -> Vec<(u32, u32, u32, i32, u32)> {
    plan.descriptors
        .iter()
        .map(|w| {
            let source = match &w.source {
                WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => slot.index,
                WriteSource::Texture { slot, .. } | WriteSource::Sampler { slot, .. } => slot.index,
                _ => u32::MAX,
            };
            (
                w.set,
                w.binding,
                w.array_element,
                avk::DescriptorType::from(w.ty).as_raw(),
                source,
            )
        })
        .collect()
}

fn region_word(plan: &BindingPlan) -> i32 {
    plan.push
        .fields
        .iter()
        .find_map(|p| match (p.field, p.location) {
            (PushField::DispatchRegion, FieldLocation::PushWindow { offset }) => Some((offset / 4) as i32),
            _ => None,
        })
        .unwrap_or(-1)
}

/// The samplers made for a dispatch's sampler writes, destroyed with it
struct Samplers<'d> {
    device: &'d ash::Device,
    made: Vec<avk::Sampler>,
}

impl Samplers<'_> {
    fn make(
        &mut self,
        compile: &metal2vulkan::device::Device,
        state: &BindState,
        source: &SamplerSource,
        image_format: Option<metal2vulkan::vk::Format>,
    ) -> Result<u64, String> {
        let SamplerSource::Slot {
            slot,
            compare,
            integer,
            opaque_black,
        } = source
        else {
            return Ok(0);
        };
        let Some(bound) = state
            .stages
            .get(&Stage::Kernel)
            .and_then(|table| table.samplers.get(&slot.index))
        else {
            return Ok(0);
        };
        let desc =
            metal2vulkan::bind::sampler_desc(compile, &bound.desc, *compare, *integer, *opaque_black, image_format)
                .map_err(e("sampler"))?;
        let mut reduction = avk::SamplerReductionModeCreateInfo::default();
        let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
        let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
        // SAFETY: `info` and its chain are live for the call.
        let sampler = unsafe { self.device.create_sampler(&info, None) }.map_err(e("sampler"))?;
        self.made.push(sampler);
        Ok(avk::Handle::as_raw(sampler))
    }
}

impl Drop for Samplers<'_> {
    fn drop(&mut self) {
        for &sampler in &self.made {
            // SAFETY: made here; NVK only read its header.
            unsafe { self.device.destroy_sampler(sampler, None) };
        }
    }
}

fn view_parts(view: &metal2vulkan::bind::ImageViewDesc) -> (u32, [u32; 4]) {
    let components = if view.shader_swizzle {
        view.components
    } else {
        TextureSwizzleChannels::DEFAULT.compose(view.components)
    };
    let c = avk::ComponentMapping::from(components);
    (
        avk::Format::from(view.format).as_raw() as u32,
        [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
    )
}

fn kernel_write(
    w: &metal2vulkan::bind::DescriptorWrite,
    commands: &DispatchCommands,
    compile: &metal2vulkan::device::Device,
    state: &BindState,
    samplers: &mut Samplers<'_>,
) -> Result<NmbDirectKernelWrite, String> {
    let mut out = NmbDirectKernelWrite {
        write: write_of(w, &commands.descriptor_ranges),
        view_format: 0,
        components: [0; 4],
        sampler: 0,
    };
    match &w.source {
        WriteSource::Texture { view, .. } => (out.view_format, out.components) = view_parts(view),
        WriteSource::Sampler {
            slot,
            compare,
            integer,
            opaque_black,
        } => {
            let source = SamplerSource::Slot {
                slot: *slot,
                compare: *compare,
                integer: *integer,
                opaque_black: *opaque_black,
            };
            out.sampler = samplers.make(compile, state, &source, None)?;
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            let format = match image {
                ImageSource::Texture { view, .. } => {
                    (out.view_format, out.components) = view_parts(view);
                    Some(view.format)
                }
                ImageSource::Neutral(_) => None,
            };
            if out.write.sampler != NMB_DIRECT_IMMUTABLE {
                out.sampler = samplers.make(compile, state, sampler, format)?;
            }
        }
        _ => {}
    }
    Ok(out)
}

/// # Safety
/// `descriptor` and `dispatch` are valid records; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_direct(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let nmb = get(device)?;
        let runtime = nmb.device();
        let compile = runtime.compile_device();
        let descriptor = compute_descriptor(descriptor)?;
        let ask = get(dispatch)?;
        let budget = Budget::standard();
        let call = Dispatch::Threadgroups {
            groups: GRID,
            threads_per_group: ask.threads,
        };
        let state = bind_state(Run::Bytes, ask)?;
        let plain = settle(compile, &descriptor, &state, &call, &budget)?;
        let moved = settle(compile, &descriptor, &bind_state(Run::Moved, ask)?, &call, &budget)?;
        let bound = settle(compile, &descriptor, &bind_state(Run::Bound, ask)?, &call, &budget)?;
        let region = |s: &Settled| s.commands.regions.first().map(|r| r.specialization.clone());
        let same = |s: &Settled| {
            s.plan.variant == plain.plan.variant
                && region(s) == region(&plain)
                && layout_of(&s.plan) == layout_of(&plain.plan)
        };
        let (push_mask, push) = push_words(&plain.commands);
        let (moved_mask, moved_push) = push_words(&moved.commands);
        let (bound_mask, bound_push) = push_words(&bound.commands);
        let ranges = |s: &Settled| -> Vec<(u32, u32, u64)> {
            s.commands
                .descriptor_ranges
                .iter()
                .map(|r| (r.set, r.binding, r.range.range))
                .collect()
        };
        let moved_same = same(&moved) && ranges(&moved) == ranges(&plain);

        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { runtime.raw() };
        let layout = Layout::new(handles.device, &plain.pipeline.layout)?;
        let mut samplers = Samplers {
            device: handles.device,
            made: Vec::new(),
        };
        let writes = plain
            .plan
            .descriptors
            .iter()
            .map(|w| kernel_write(w, &plain.commands, compile, &state, &mut samplers))
            .collect::<Result<Vec<_>, String>>()?;
        let first = plain.commands.regions.first();
        let specialization = first.map(|r| r.specialization.clone()).unwrap_or_default();
        let map: Vec<avk::SpecializationMapEntry> = specialization.entries.iter().map(|&m| m.into()).collect();
        let info = avk::SpecializationInfo::default()
            .map_entries(&map)
            .data(&specialization.data);
        let stage = &plain.pipeline.stage;
        let entry = CString::new(stage.entry_point.as_str()).unwrap_or_default();
        let variant = CString::new(format!("{:?}", plain.plan.variant)).unwrap_or_default();
        let kernel = NmbDirectKernel {
            device: avk::Handle::as_raw(handles.device.handle()),
            layout: avk::Handle::as_raw(layout.pipeline),
            set0: layout.set0(),
            stage: NmbDirectStage {
                spirv: stage.spirv.as_ptr(),
                spirv_bytes: stage.spirv.len() * 4,
                entry: entry.as_ptr(),
                stage: avk::ShaderStageFlags::from(stage.stage).as_raw(),
                flags: first.map_or(0, |r| avk::PipelineShaderStageCreateFlags::from(r.stage_flags).as_raw()),
                subgroup_size: first.and_then(|r| r.required_subgroup_size).unwrap_or(0),
                specialization: if map.is_empty() {
                    std::ptr::null()
                } else {
                    (&info as *const avk::SpecializationInfo<'_>).cast()
                },
            },
            writes: writes.as_ptr(),
            write_count: writes.len() as u32,
            push_mask,
            push,
            moved_mask,
            moved: moved_push,
            bound_mask,
            bound: bound_push,
            moved_same,
            bound_same: same(&bound),
            region_word: region_word(&plain.plan),
            regions: plain.commands.regions.len() as u32,
            push_holes: plain.commands.push_holes.len() as u32,
            variant: variant.as_ptr(),
        };
        // SAFETY: the caller's contract; everything `kernel` points at lives until after the call.
        Ok(unsafe { each(&kernel, ctx) })
    })
}
