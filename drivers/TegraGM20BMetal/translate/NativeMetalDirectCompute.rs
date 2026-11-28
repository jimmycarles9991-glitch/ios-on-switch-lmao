// SPDX-License-Identifier: MIT
//! The driver's direct compile of a compute state at one dispatch shape (NativeMetalDirect.h): what m2v's
//! translator makes of the kernel for the three dispatches the pack's kernel probe ran
//! (drivers/TegraGM20BMetal/pack/kernels.py), with no pipeline state, command buffer or capture. Only the
//! translator's public API is used. A runtime device lends its compile device and its Vulkan device, on which the
//! variant's layout and samplers are made too; the offline compile (the pack generator's, and the compile context's:
//! NativeMetalCompile.rs) runs on the compile profile's native device with the layout as data alone.

use crate::descriptors::{DescriptorFunction, NmbComputePipelineDescriptorOf, compute_descriptor, e, get, guard};
use crate::direct::compile_error;
use crate::direct_abi::{HEAP_ARRAYS, NmbDirectStage};
use crate::direct_compute_abi::{NmbDirectDispatch, NmbDirectKernel};
use crate::direct_compute_binds::{Run, bind_state, dropped_stores, mapped_samplers};
use crate::direct_compute_words::{
    access_masks, arguments_of, guest_bases, layout_of, push_words, record_residuals, region_word, stage_in_words,
};
use crate::direct_compute_writes::{Samplers, kernel_write};
use crate::direct_layout::Layout;
use crate::direct_writes::{residual_words, spill_table};
use crate::offline::{Devices, NmbOfflineComputePipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{BindState, BindingPlan, Dispatch, DispatchCommands};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::pipeline::{
    ComputePipeline, ComputePipelineDescriptor, NativePipeline, NativeProgram, NativeRecord, SlotClass, SlotInterface,
    Stage, StageSource,
};
use std::ffi::{CString, c_void};
use std::sync::Arc;

const GRID: [u32; 3] = [3, 5, 2];

/// The kernel compiled under variant `key`, as the runtime compiles a state's variant: the function under the key's lowering
/// inputs with the descriptor's stage input and call stack depth, then the pipeline of it
pub fn compiled(
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

/// A kernel's compiled variants a caller already holds (the m2v runtime's compute artifact), taken in place of compiling
/// them again
pub trait KernelVariants {
    fn pipeline(&self, key: &VariantKey, budget: &Budget) -> Result<Arc<NativeComputePipeline>, String>;
}

#[derive(Clone)]
pub struct NativeComputePipeline {
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
    state: BindState,
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    (ask, state): (&NmbDirectDispatch, &BindState),
    dispatch: &Dispatch,
    budget: &Budget,
    artifact: Option<&dyn KernelVariants>,
) -> Result<Settled, String> {
    let pipeline = |key: &VariantKey| match artifact {
        Some(artifact) => artifact.pipeline(key, budget).map(|p| (*p).clone()),
        None => compiled(compile, descriptor, key, budget),
    };
    let base = pipeline(&VariantKey::new())?;
    let access = |slot: u32| match base
        .interface
        .slots
        .iter()
        .find(|(s, _)| s.stage == Stage::Kernel && s.class == SlotClass::Texture && s.index == slot)
    {
        Some((_, SlotInterface::Texture(t))) => Some(t.access),
        _ => None,
    };
    let state = dropped_stores(access, ask, mapped_samplers(&base.layout, ask, state)?)?;
    let shape = state.shape();
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
        .instantiate_dispatch(&state, dispatch)
        .map_err(e("instantiate dispatch"))?;
    Ok(Settled {
        pipeline,
        plan,
        commands,
        state,
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
pub fn compute_offline<F: DescriptorFunction>(
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
            &crate::compile_budget(),
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

pub fn direct(
    vulkan: Option<&ash::Device>,
    compile: &metal2vulkan::device::Device,
    descriptor: &ComputePipelineDescriptor,
    artifact: Option<&dyn KernelVariants>,
    ask: &NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let budget = crate::compile_budget();
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
    let plain = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Bytes, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    let moved = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Moved, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    let bound = settle(
        compile,
        descriptor,
        (ask, &bind_state(Run::Bound, ask, stage_in)?),
        &call,
        &budget,
        artifact,
    )?;
    // InlineBytes have no buffer hazard entries; the moved and bound plans expose those accesses.
    let [buffer_reads, buffer_writes, texture_reads, texture_writes] =
        access_masks([&plain.plan, &moved.plan, &bound.plan]);
    let region = |s: &Settled| s.commands.regions.first().map(|r| r.specialization.clone());
    let same = |s: &Settled| {
        s.plan.variant == plain.plan.variant
            && region(s) == region(&plain)
            && layout_of(&s.plan) == layout_of(&plain.plan)
    };
    let residual_word = record_residuals(
        [&moved.plan.variant, &bound.plan.variant],
        region(&bound) != region(&moved),
        &[layout_of(&moved.plan), layout_of(&bound.plan)],
        residual_words(&plain.plan, Stage::Kernel),
    )?;
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
        .map(|w| {
            let parts = (&plain.commands, &plain.pipeline.layout);
            kernel_write(w, parts, compile, ask, &plain.state, &mut samplers)
        })
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
        residual_word,
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
    kernel.other_holes += [&plain, &moved, &bound]
        .iter()
        .map(|s| guest_bases(s.plan.push.fields.iter().map(|p| &p.field)))
        .max()
        .unwrap_or(0);
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
