// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile of a compute state at one dispatch shape (NativeMetalDirect.h): what m2v's
//! translator makes of the kernel for the three dispatches the pack's kernel probe ran
//! (drivers/TegraGM20BMetal/raw/pack/kernels.py), with no pipeline state, command buffer or capture. Only the
//! translator's public API is used. A runtime device lends its compile device and its Vulkan device, on which the
//! variant's layout and samplers are made too; the offline compile (the pack generator's, and the compile context's:
//! NativeMetalCompile.rs) runs on the compile profile's native device with the layout as data alone.

use crate::compute_artifact::NmbComputeArtifact;
use crate::direct::{
    HEAP_ARRAYS, Layout, NMB_DIRECT_IMMUTABLE, NmbDirectBinding, NmbDirectSamplerState, NmbDirectStage, NmbDirectWrite,
    compile_error, residual_words, sampler_state, spill_table, write_of,
};
use crate::objects::{
    DescriptorFunction, NmbComputePipelineDescriptor, NmbComputePipelineDescriptorOf, NmbDevice, NmbSamplerDescriptor,
    compute_descriptor, e, get, guard, raw, sampler_descriptor,
};
use crate::offline::{Devices, NmbOfflineComputePipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, Dispatch, DispatchCommands, FieldLocation,
    ImageSource, NeutralResource, PushHole, SamplerSource, StageInRegion, StageTable, TextureFacts, UseTarget,
    WriteSource, heap_class_code,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::metal::{PixelFormat, TextureSwizzleChannels, TextureType, TextureUsage};
use metal2vulkan::pipeline::{
    BindingSource, ComputePipeline, ComputePipelineDescriptor, NativePipeline, NativeProgram, NativeRecord,
    PipelineLayout, PushField, SlotClass, Stage, StageSource,
};
use std::ffi::{CString, c_void};
use std::sync::Arc;

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
    pub region_mode: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectKernelWrite {
    pub write: NmbDirectWrite,
    pub view_format: u32,
    pub components: [u32; 4],
    pub sampler: u64,
    pub sampler_state: NmbDirectSamplerState,
    pub has_sampler: bool,
}

#[repr(C)]
pub struct NmbDirectKernel {
    pub device: u64,
    pub layout: u64,
    pub layout_data: *const crate::direct::NmbDirectLayout,
    pub set0: u64,
    pub set0_bindings: *const NmbDirectBinding,
    pub set0_binding_count: u32,
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
    pub buffer_reads: u32,
    pub buffer_writes: u32,
    pub texture_reads: u32,
    pub texture_writes: u32,
    pub root_table_word: u8,
    pub status_word: u8,
    pub other_holes: u32,
    pub root_slots: u32,
    pub set1: u64,
    pub heap_arrays: u32,
    pub heap_class: [u32; HEAP_ARRAYS],
    pub spill: *const u32,
    pub moved_spill: *const u32,
    pub bound_spill: *const u32,
    pub spill_words: u32,
    pub moved_spill_words: u32,
    pub bound_spill_words: u32,
    pub residual_word: [u8; 32],
    pub stage_in_region_word: i32,
    pub stage_in_strides: [[u32; 2]; 4],
}

/// The kernel's reads through argument buffers the raw path fills (NativeMetalDirect.rs holes_of and heap_of for a draw): its
/// root-table and ExecutionStatus address holes (push word + 1), the others counted, the buffer slots its root table holds
/// an entry for as the bound dispatch names them, and its set 1's heap arrays
fn arguments_of(
    commands: &DispatchCommands,
    bound: &DispatchCommands,
    layout: &PipelineLayout,
    sets: &Layout<'_>,
    kernel: &mut NmbDirectKernel,
) -> Result<(), String> {
    for hole in &commands.push_holes {
        let (word, offset) = match *hole {
            PushHole::RootTableAddress { offset, .. } => (&mut kernel.root_table_word, offset),
            PushHole::ExecutionStatusAddress { offset, .. } => (&mut kernel.status_word, offset),
            _ => {
                kernel.other_holes += 1;
                continue;
            }
        };
        if offset % 4 != 0 || offset > 128 - 8 {
            return Err("an address push hole outside the root push window".into());
        }
        let encoded = (offset / 4 + 1) as u8;
        if *word != 0 && *word != encoded {
            return Err("the kernel's address push hole at two places".into());
        }
        *word = encoded;
    }
    if let Some(table) = &bound.root_table {
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
            if root - 2 >= 31 {
                return Err("a root table entry past the buffer slots".into());
            }
            kernel.root_slots |= 1 << (root - 2);
        }
    }
    let Some((at, set)) = layout.sets.iter().enumerate().find(|(_, s)| s.set == 1) else {
        return Ok(());
    };
    kernel.set1 = sets.set_at(at);
    kernel.heap_arrays = set.bindings.len() as u32;
    for b in &set.bindings {
        let code = match &b.source {
            BindingSource::Heap(class) => heap_class_code(class).unwrap_or(0),
            _ => 0,
        };
        if let Some(slot) = kernel.heap_class.get_mut(b.binding as usize) {
            *slot = code;
        }
    }
    Ok(())
}

fn access_masks(plans: [&BindingPlan; 3]) -> [u32; 4] {
    let mut masks = [0u32; 4];
    for use_ in plans.into_iter().flat_map(|p| &p.uses) {
        let UseTarget::Slot(slot) = use_.resource else {
            if use_.access.write {
                masks[1] = u32::MAX;
                masks[3] = u32::MAX;
            }
            continue;
        };
        if slot.stage != Stage::Kernel || slot.index >= 32 {
            continue;
        }
        let base = match slot.class {
            SlotClass::Buffer => 0,
            SlotClass::Texture => 2,
            SlotClass::Sampler => continue,
        };
        masks[base] |= u32::from(use_.access.read) << slot.index;
        masks[base + 1] |= u32::from(use_.access.write) << slot.index;
    }
    masks
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

fn bind_state(run: Run, dispatch: &NmbDirectDispatch, stage_in: bool) -> Result<BindState, String> {
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
    if stage_in {
        state.stage_in_region = Some(StageInRegion::new([0; 3], [1; 3]));
    }
    Ok(state)
}

/// The kernel compiled under variant `key`, as the runtime compiles a state's variant: the function under the key's lowering
/// inputs with the descriptor's stage input and call stack depth, then the pipeline of it
pub(crate) fn compiled(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    key: &VariantKey,
    budget: &Budget,
) -> Result<NativeComputePipeline, String> {
    let StageSource::Function(function) = &descriptor.kernel else {
        return Err("a kernel compiled already".into());
    };
    let mut inputs = key.lowering_inputs(Stage::Kernel);
    inputs.stage_input = descriptor.stage_input.clone();
    inputs.max_call_stack_depth = Some(descriptor.max_call_stack_depth).filter(|&k| k > 1);
    let function = compile
        .compile_native_function(function, &inputs, budget)
        .map_err(compile_error("compile kernel"))?;
    let pipeline = function
        .into_pipeline(compile, descriptor)
        .map_err(compile_error("link native kernel"))?;
    Ok(NativeComputePipeline {
        pipeline: Arc::new(pipeline),
    })
}

#[derive(Clone)]
pub(crate) struct NativeComputePipeline {
    pub pipeline: Arc<NativePipeline>,
}

impl std::ops::Deref for NativeComputePipeline {
    type Target = ComputePipeline;
    fn deref(&self) -> &Self::Target {
        &self.pipeline.metadata
    }
}

struct Settled {
    pipeline: NativeComputePipeline,
    plan: BindingPlan,
    commands: DispatchCommands,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    state: &BindState,
    dispatch: &Dispatch,
    budget: &Budget,
    artifact: Option<&NmbComputeArtifact>,
) -> Result<Settled, String> {
    let shape = state.shape();
    let pipeline = |key: &VariantKey| match artifact {
        Some(artifact) => artifact.pipeline(key, budget).map(|p| (*p).clone()),
        None => compiled(compile, descriptor, key, budget),
    };
    let base = pipeline(&VariantKey::new())?;
    let plan = base.plan(compile, &shape).map_err(e("plan"))?;
    let wanted = plan.dispatch_variant(dispatch).map_err(e("dispatch variant"))?;
    let (pipeline, plan) = if wanted == VariantKey::new() {
        (base, plan)
    } else {
        let pipeline = pipeline(&wanted)?;
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

/// A stage-in kernel's push words the raw dispatch writes: its StageInRegion's and each dynamic layout's StageInStride
fn stage_in_words(plan: &BindingPlan) -> Result<(i32, [[u32; 2]; 4]), String> {
    let (mut region, mut strides, mut n) = (-1, [[0u32; 2]; 4], 0);
    for p in &plan.push.fields {
        let FieldLocation::PushWindow { offset } = p.location else {
            continue;
        };
        match p.field {
            PushField::StageInRegion => region = (offset / 4) as i32,
            PushField::StageInStride(index) => {
                if n == strides.len() {
                    return Err("more than four stage-in layouts of a dynamic stride".into());
                }
                strides[n] = [index, offset / 4 + 1];
                n += 1;
            }
            _ => {}
        }
    }
    Ok((region, strides))
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

/// The samplers a dispatch's sampler writes take: their states, and the Vulkan samplers made of them where there is a
/// device, destroyed with it
struct Samplers<'d> {
    device: Option<&'d ash::Device>,
    made: Vec<avk::Sampler>,
}

impl Samplers<'_> {
    /// The sampler a write takes (its handle, 0 with no device, and its state); none where the dispatch binds none
    fn make(
        &mut self,
        compile: &metal2vulkan::device::Device,
        state: &BindState,
        source: &SamplerSource,
        image_format: Option<metal2vulkan::vk::Format>,
    ) -> Result<Option<(u64, NmbDirectSamplerState)>, String> {
        if let SamplerSource::Neutral(NeutralResource::Sampler { compare }) = source {
            let info = avk::SamplerCreateInfo::default()
                .mag_filter(avk::Filter::NEAREST)
                .min_filter(avk::Filter::NEAREST)
                .mipmap_mode(avk::SamplerMipmapMode::NEAREST)
                .address_mode_u(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_v(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_w(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .max_lod(avk::LOD_CLAMP_NONE)
                .compare_enable(*compare)
                .compare_op(avk::CompareOp::NEVER);
            let state = sampler_state(&info, avk::SamplerReductionMode::WEIGHTED_AVERAGE, None);
            return Ok(Some((self.create(&info)?, state)));
        }
        let SamplerSource::Slot {
            slot,
            compare,
            integer,
            opaque_black,
        } = source
        else {
            return Ok(None);
        };
        let Some(bound) = state
            .stages
            .get(&Stage::Kernel)
            .and_then(|table| table.samplers.get(&slot.index))
        else {
            return Ok(None);
        };
        let desc =
            metal2vulkan::bind::sampler_desc(compile, &bound.desc, *compare, *integer, *opaque_black, image_format)
                .map_err(e("sampler"))?;
        let mut reduction = avk::SamplerReductionModeCreateInfo::default();
        let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
        let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
        let made = sampler_state(
            &info,
            desc.reduction.into(),
            desc.border_components.map(avk::ComponentMapping::from),
        );
        Ok(Some((self.create(&info)?, made)))
    }

    fn create(&mut self, info: &avk::SamplerCreateInfo<'_>) -> Result<u64, String> {
        let Some(device) = self.device else {
            return Ok(0);
        };
        // SAFETY: `info` and its chain are live for the call.
        let sampler = unsafe { device.create_sampler(info, None) }.map_err(e("sampler"))?;
        self.made.push(sampler);
        Ok(avk::Handle::as_raw(sampler))
    }
}

impl Drop for Samplers<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        for &sampler in &self.made {
            // SAFETY: made here; NVK only read its header.
            unsafe { device.destroy_sampler(sampler, None) };
        }
    }
}

impl NmbDirectKernelWrite {
    fn take_sampler(&mut self, made: Option<(u64, NmbDirectSamplerState)>) {
        if let Some((sampler, state)) = made {
            (self.sampler, self.sampler_state, self.has_sampler) = (sampler, state, true);
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
        sampler_state: NmbDirectSamplerState::default(),
        has_sampler: false,
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
            out.take_sampler(samplers.make(compile, state, &source, None)?);
        }
        WriteSource::Neutral(neutral @ NeutralResource::Sampler { .. }) => {
            out.take_sampler(samplers.make(compile, state, &SamplerSource::Neutral(*neutral), None)?);
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
                out.take_sampler(samplers.make(compile, state, sampler, format)?);
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
        let native = crate::direct::native_compute_device(nmb)?;
        let descriptor = compute_descriptor(descriptor)?;
        let ask = get(dispatch)?;
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { nmb.device().raw() };
        direct(Some(handles.device), &native, &descriptor, None, ask, each, ctx)
    })
}

/// nmb_compute_pipeline_direct's answer for a kernel of offline functions, made on the compile profile's device with no
/// Vulkan object: every handle the kernel hands over is 0.
///
/// # Safety
/// As nmb_compute_pipeline_direct's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_offline(
    descriptor: *const NmbOfflineComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        compute_offline(offline_devices()?, descriptor, dispatch, each, ctx)
    })
}

/// nmb_compute_pipeline_offline's compile on `devices`, of a descriptor of either function handle: the pack generator's
/// offline functions, or the compile context's (NativeMetalCompile.rs)
pub(crate) fn compute_offline<F: DescriptorFunction>(
    devices: &Devices,
    descriptor: *const NmbComputePipelineDescriptorOf<F>,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let descriptor = compute_descriptor(descriptor)?;
    direct(None, &devices.native, &descriptor, None, get(dispatch)?, each, ctx)
}

/// The answers the runtime's compute prepare gives a state of offline functions (nmb_compute_artifact_new's metal_info,
/// on the compile profile's native device), into `facts`: maxTotalThreadsPerThreadgroup, threadExecutionWidth,
/// staticThreadgroupMemoryLength
///
/// # Safety
/// The descriptor and its function handles are live for this call; `facts` holds three words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_compute_pipeline_facts(
    descriptor: *const NmbOfflineComputePipelineDescriptor,
    facts: *mut u32,
) -> bool {
    guard(false, || {
        if facts.is_null() {
            return Err("null compute facts".into());
        }
        let descriptor = compute_descriptor(descriptor)?;
        let made = compiled(
            &offline_devices()?.native,
            &descriptor,
            &VariantKey::new(),
            &Budget::standard(),
        )?;
        let info = &made.metal_info;
        // SAFETY: the caller's three words.
        unsafe { std::slice::from_raw_parts_mut(facts, 3) }.copy_from_slice(&[
            info.max_total_threads_per_threadgroup,
            info.thread_execution_width,
            info.static_threadgroup_memory_length,
        ]);
        Ok(true)
    })
}

/// # Safety
/// The artifact and dispatch are live; the callback is valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_direct(
    device: *mut NmbDevice,
    artifact: *const NmbComputeArtifact,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let artifact = get(artifact)?;
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { get(device)?.device().raw() };
        direct(
            Some(handles.device),
            &artifact.compile,
            &artifact.descriptor,
            Some(artifact),
            get(dispatch)?,
            each,
            ctx,
        )
    })
}

fn direct(
    vulkan: Option<&ash::Device>,
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    artifact: Option<&NmbComputeArtifact>,
    ask: &NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let budget = Budget::standard();
    let call = if ask.region_mode != 0 {
        Dispatch::ThreadgroupRange {
            groups: GRID,
            first: [0; 3],
            count: GRID,
            threads_per_group: ask.threads,
        }
    } else {
        Dispatch::Threadgroups {
            groups: GRID,
            threads_per_group: ask.threads,
        }
    };
    let stage_in = descriptor.stage_input.is_some();
    let state = bind_state(Run::Bytes, ask, stage_in)?;
    let plain = settle(compile, descriptor, &state, &call, &budget, artifact)?;
    let moved = settle(
        compile,
        descriptor,
        &bind_state(Run::Moved, ask, stage_in)?,
        &call,
        &budget,
        artifact,
    )?;
    let bound = settle(
        compile,
        descriptor,
        &bind_state(Run::Bound, ask, stage_in)?,
        &call,
        &budget,
        artifact,
    )?;
    let region = |s: &Settled| s.commands.regions.first().map(|r| r.specialization.clone());
    let same = |s: &Settled| {
        s.plan.variant == plain.plan.variant
            && region(s) == region(&plain)
            && layout_of(&s.plan) == layout_of(&plain.plan)
    };
    let (push_mask, push) = push_words(&plain.commands);
    let (stage_in_region_word, stage_in_strides) = stage_in_words(&plain.plan)?;
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
    let spill = |s: &Settled| spill_table(&s.plan, &s.commands.descriptor_bytes, Stage::Kernel);
    let (plain_spill, moved_spill, bound_spill) = (spill(&plain), spill(&moved), spill(&bound));

    let layout = Layout::new(vulkan, &plain.pipeline.layout)?;
    let mut samplers = Samplers {
        device: vulkan,
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
    let semantic = plain.pipeline.pipeline.program.as_ref().map(SemanticView::of);
    // InlineBytes have no buffer hazard entries; the moved and bound plans expose those accesses.
    let [buffer_reads, buffer_writes, texture_reads, texture_writes] =
        access_masks([&plain.plan, &moved.plan, &bound.plan]);
    let mut kernel = NmbDirectKernel {
        device: vulkan.map_or(0, |d| avk::Handle::as_raw(d.handle())),
        layout: avk::Handle::as_raw(layout.pipeline),
        layout_data: &layout.data,
        set0: layout.set0(),
        set0_bindings: layout.set0_bindings.as_ptr(),
        set0_binding_count: layout.set0_bindings.len() as u32,
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
            semantic: semantic
                .as_ref()
                .map_or(std::ptr::null(), |p| (p as *const SemanticView).cast()),
            portable_context: Arc::as_ptr(&plain.pipeline.pipeline).cast(),
            portable: Some(materialize_portable),
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
        buffer_reads,
        buffer_writes,
        texture_reads,
        texture_writes,
        root_table_word: 0,
        status_word: 0,
        other_holes: 0,
        root_slots: 0,
        set1: 0,
        heap_arrays: 0,
        heap_class: [0; HEAP_ARRAYS],
        spill: plain_spill.as_ptr(),
        moved_spill: moved_spill.as_ptr(),
        bound_spill: bound_spill.as_ptr(),
        spill_words: plain_spill.len() as u32,
        moved_spill_words: moved_spill.len() as u32,
        bound_spill_words: bound_spill.len() as u32,
        residual_word: residual_words(&plain.plan, Stage::Kernel),
        stage_in_region_word,
        stage_in_strides,
    };
    arguments_of(
        &plain.commands,
        &bound.commands,
        &plain.pipeline.layout,
        &layout,
        &mut kernel,
    )?;
    // SAFETY: the caller's contract; everything `kernel` points at lives until after the call.
    Ok(unsafe { each(&kernel, ctx) })
}

unsafe extern "C" fn materialize_portable(context: *const c_void, words: *mut *const u32, bytes: *mut usize) -> bool {
    guard(false, || {
        let pipeline = get(context.cast::<NativePipeline>())?;
        let portable = pipeline
            .portable_words()
            .map_err(compile_error("portable native fallback"))?;
        // SAFETY: NVK supplies both output fields, and the pipeline owns the returned words through the callback.
        unsafe {
            *words.as_mut().ok_or("null portable words")? = portable.as_ptr();
            *bytes.as_mut().ok_or("null portable size")? = portable.len() * 4;
        }
        Ok(true)
    })
}

#[repr(C)]
struct SemanticView {
    version: u32,
    types: u32,
    values: u32,
    blocks: u32,
    entry: u32,
    record_count: u32,
    operand_count: u32,
    records: *const NativeRecord,
    operands: *const u32,
}

impl SemanticView {
    fn of(program: &NativeProgram) -> Self {
        Self {
            version: NativeProgram::VERSION,
            types: program.types,
            values: program.values,
            blocks: program.blocks,
            entry: program.entry,
            record_count: program.records.len() as u32,
            operand_count: program.operands.len() as u32,
            records: program.records.as_ptr(),
            operands: program.operands.as_ptr(),
        }
    }
}
