// SPDX-License-Identifier: MIT
//! The C descriptors the direct compile takes (NativeMetalObjects.h, NativeMetalDirect.h), as the translator's own
//! descriptors, generic over the function handle (the runtime's or the offline compile's), and the per-thread error
//! every C call leaves its failure in (nmb_last_error).

use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues};
use metal2vulkan::metal::{
    AttributeFormat, BlendFactor, BlendOperation, ColorWriteMask, CompareFunction, DataType, IndexType, PixelFormat,
    PrimitiveTopologyClass, SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter,
    StepFunction, TessellationControlPointIndexType, TessellationFactorFormat, TessellationFactorStepFunction,
    TessellationPartitionMode, VertexFormat, VertexStepFunction, Winding,
};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, ComputePipelineDescriptor, FunctionDescriptor, LinkedFunction, LinkedFunctions,
    RenderPipelineDescriptor, StageInputAttributeDescriptor, StageInputDescriptor, StageInputLayoutDescriptor,
    StageSource, TessellationDescriptor, VertexAttributeDescriptor, VertexBufferLayoutDescriptor, VertexDescriptor,
};
use std::cell::RefCell;
use std::ffi::{CStr, c_char, c_void};

thread_local! {
    static LAST: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

pub fn set_error(why: String) {
    LAST.with(|last| {
        let mut last = last.borrow_mut();
        last.clear();
        last.extend(why.bytes().filter(|b| *b != 0));
        last.push(0);
    });
}

/// Runs `body`, turning an error or a panic into the per-thread reason and `fallback`.
pub fn guard<T>(fallback: T, body: impl FnOnce() -> Result<T, String>) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(why)) => {
            set_error(why);
            fallback
        }
        Err(_) => {
            set_error("panic in the Metal runtime".into());
            fallback
        }
    }
}

/// The object a handle points at.
pub fn get<'a, T>(handle: *const T) -> Result<&'a T, String> {
    // SAFETY: handles come from `boxed` and live until their release call (the header's contract).
    unsafe { handle.as_ref() }.ok_or_else(|| "null handle".to_string())
}

pub fn release<T>(handle: *mut T) {
    if !handle.is_null() {
        // SAFETY: from `boxed`, released once.
        drop(unsafe { Box::from_raw(handle) });
    }
}

pub fn copy_out(text: &str, out: *mut c_char, size: usize) -> usize {
    if !out.is_null() && size > 0 {
        let n = text.len().min(size - 1);
        // SAFETY: the caller's buffer holds `size` bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, n);
            *out.add(n) = 0;
        }
    }
    text.len()
}

pub fn raw<T: TryFrom<u32>>(value: u64, what: &str) -> Result<T, String>
where
    T::Error: std::fmt::Debug,
{
    u32::try_from(value)
        .ok()
        .and_then(|v| T::try_from(v).ok())
        .ok_or_else(|| format!("{what} {value} is not a Metal value"))
}

/// The error of `what` as the caller's text, and m2v's refusal of work iPadOS sent, logged once per call and kind of error
/// in the process (kits/SwitchRefusal: an error line, kept in release). The kind is the error's variant and first field
/// (`NotImplemented(Format`, `Unsupported(Unsupported { feature: FramebufferFetch`) without digits, so a value that varies
/// per call makes no line of its own.
pub fn e<E: std::fmt::Debug>(what: &'static str) -> impl FnOnce(E) -> String {
    move |error| {
        let text = format!("{what}: {error:?}");
        let kind: String = text[what.len() + 2..]
            .split([',', ')'])
            .next()
            .unwrap_or_default()
            .chars()
            .filter(|c| !c.is_ascii_digit())
            .collect();
        let key = switch_refusal::key_with(switch_refusal::key("m2v", what), switch_refusal::key("", &kind));
        switch_refusal::refuse(c"m2v", what, key, || text.clone());
        text
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_last_error() -> *const c_char {
    LAST.with(|last| {
        let last = last.borrow();
        if last.is_empty() {
            c"".as_ptr()
        } else {
            last.as_ptr() as *const c_char
        }
    })
}

#[repr(C)]
pub struct NmbConstant {
    pub kind: u64,
    pub index: u64,
    pub name: *const c_char,
    pub bytes: *const c_void,
    pub length: u64,
}

/// nmb_function_new_specialized's arguments: the function's name, its constant values and the name it links under
///
/// # Safety
/// As nmb_function_new_specialized's.
pub unsafe fn specialization<'a>(
    name: *const c_char,
    specialized_name: *const c_char,
    constants: *const NmbConstant,
    count: u32,
) -> Result<(&'a str, FunctionConstantValues, Option<String>), String> {
    if name.is_null() || (constants.is_null() && count > 0) {
        return Err("null function name or constants".into());
    }
    // SAFETY: the caller's contract.
    let name = unsafe { CStr::from_ptr(name) }.to_str().map_err(e("function name"))?;
    let constants = if count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(constants, count as usize) }
    };
    let mut values = FunctionConstantValues::defaults();
    for c in constants {
        if c.bytes.is_null() {
            return Err("constant without bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(c.bytes as *const u8, c.length as usize) };
        let value = FunctionConstantValue::new(raw::<DataType>(c.kind, "constant type")?, bytes.to_vec());
        if c.name.is_null() {
            values
                .by_index
                .insert(u32::try_from(c.index).map_err(e("constant index"))?, value);
        } else {
            // SAFETY: the caller's contract.
            let key = unsafe { CStr::from_ptr(c.name) }.to_str().map_err(e("constant name"))?;
            values.by_name.insert(key.to_string(), value);
        }
    }
    let specialized = if specialized_name.is_null() {
        None
    } else {
        // SAFETY: the caller's contract.
        let s = unsafe { CStr::from_ptr(specialized_name) }
            .to_str()
            .map_err(e("specialized name"))?;
        Some(s.to_string())
    };
    Ok((name, values, specialized))
}

/// Attribute `at` of `attributes` as nmb_function_attribute answers it
pub fn attribute_out(
    attributes: &[metal2vulkan::reflection::Attribute],
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> Result<usize, String> {
    if info.is_null() {
        return Err("null attribute info".into());
    }
    let a = attributes
        .get(at as usize)
        .ok_or_else(|| format!("no attribute {at}"))?;
    // SAFETY: the header's five words.
    unsafe { std::slice::from_raw_parts_mut(info, 5) }.copy_from_slice(&[
        u64::from(a.attribute_index()),
        u64::from(a.attribute_type().as_raw()),
        u64::from(a.is_active()),
        u64::from(a.is_patch_data()),
        u64::from(a.is_patch_control_point_data()),
    ]);
    Ok(copy_out(a.name(), out, size))
}

/// A pipeline descriptor's function handle as the translator takes it: the runtime's function, or the offline compile's
/// (offline.rs)
pub trait DescriptorFunction {
    fn stage_function(&self) -> FunctionDescriptor;
    /// The function as a stage links it, under its specialized name
    fn linked_function(&self) -> LinkedFunction;
}

#[repr(C)]
pub struct SwitchLinkedGroupName {
    pub bytes: *const c_char,
    pub length: u64,
}

#[repr(C)]
pub struct NmbComputePipelineDescriptorOf<F> {
    pub function: *mut F,
    pub max_threads: u64,
    pub multiple_of_execution_width: u64,
    pub indirect: u64,
    pub max_call_stack_depth: u64,
    pub adding_binary_functions: u64,
    pub mutability: [u64; 31],
    pub linked: *const *mut F,
    pub linked_count: u64,
    pub private_linked: *const *mut F,
    pub private_count: u64,
    pub binary_linked: *const *mut F,
    pub binary_count: u64,
    pub stage_inputs: u64,
    pub stage_attributes: [NmbVertexAttribute; 31],
    pub stage_layouts: [NmbVertexLayout; 31],
    pub stage_index_type: u64,
    pub stage_index_buffer: u64,
    pub group_linked: *const *mut F,
    pub group_names: *const SwitchLinkedGroupName,
    pub group_count: u64,
}

/// A compute descriptor's stage input (`stageInputDescriptor`): its set attributes, their layouts (a dynamic stride as
/// `None`), its index type and index buffer
pub fn stage_input<F>(d: &NmbComputePipelineDescriptorOf<F>) -> Result<StageInputDescriptor, String> {
    let mut out = StageInputDescriptor::new();
    for (i, a) in d.stage_attributes.iter().enumerate() {
        if a.format == 0 {
            continue;
        }
        let buffer = u32::try_from(a.buffer).map_err(e("stage input buffer"))?;
        out.attributes.insert(
            i as u32,
            StageInputAttributeDescriptor::new(
                raw::<AttributeFormat>(a.format, "attribute format")?,
                u32::try_from(a.offset).map_err(e("stage input offset"))?,
                buffer,
            ),
        );
        let l = d
            .stage_layouts
            .get(buffer as usize)
            .ok_or("a stage input buffer past 30")?;
        let stride = (l.stride != u64::MAX).then_some(l.stride as u32);
        let mut layout = StageInputLayoutDescriptor::new(stride);
        layout.step_function = raw::<StepFunction>(l.step_function, "step function")?;
        layout.step_rate = l.step_rate as u32;
        out.layouts.insert(buffer, layout);
    }
    out.index_type = raw::<IndexType>(d.stage_index_type, "stage input index type")?;
    out.index_buffer_index = u32::try_from(d.stage_index_buffer).map_err(e("stage input index buffer"))?;
    Ok(out)
}

/// The functions of a counted handle array, each as a stage links it
pub fn linked_functions<F: DescriptorFunction>(
    functions: *const *mut F,
    count: u64,
) -> Result<Vec<LinkedFunction>, String> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if functions.is_null() {
        return Err("null linked functions".into());
    }
    // SAFETY: the header's counted array of live function handles.
    let handles = unsafe { std::slice::from_raw_parts(functions, usize::try_from(count).map_err(e("linked count"))?) };
    handles.iter().map(|&f| Ok(get(f)?.linked_function())).collect()
}

pub fn compute_descriptor<F: DescriptorFunction>(
    descriptor: *const NmbComputePipelineDescriptorOf<F>,
) -> Result<ComputePipelineDescriptor, String> {
    let d = get(descriptor)?;
    // MTLLinkedFunctions' functions and privateFunctions: the external functions the kernel calls by name (MPS's ND-array kernels
    // import their prologue and epilogue functions, prefixPrimary_f and the like, this way).
    let mut linked = LinkedFunctions::new();
    linked.functions = linked_functions(d.linked, d.linked_count)?;
    linked.private_functions = linked_functions(d.private_linked, d.private_count)?;
    linked.binary_functions = linked_functions(d.binary_linked, d.binary_count)?
        .into_iter()
        .map(|f| f.with_precompiled(true))
        .collect();
    if d.group_count != 0 {
        if d.group_names.is_null() {
            return Err("null linked group names".into());
        }
        let functions = linked_functions(d.group_linked, d.group_count)?;
        // SAFETY: the descriptor's counted array of names, one for each grouped function.
        let names = unsafe { std::slice::from_raw_parts(d.group_names, functions.len()) };
        for (name, function) in names.iter().zip(functions) {
            if name.bytes.is_null() && name.length != 0 {
                return Err("null linked group name".into());
            }
            let length = usize::try_from(name.length).map_err(e("group name length"))?;
            let bytes = if length == 0 {
                &[]
            } else {
                // SAFETY: the descriptor's group names have their declared byte lengths.
                unsafe { std::slice::from_raw_parts(name.bytes.cast::<u8>(), length) }
            };
            let name = std::str::from_utf8(bytes).map_err(e("group name"))?;
            linked.groups.entry(name.to_owned()).or_default().push(function);
        }
    }
    let mut out = ComputePipelineDescriptor::new(StageSource::Function(
        get(d.function)?.stage_function().with_linked(linked),
    ));
    out.max_total_threads_per_threadgroup = if d.max_threads == 0 {
        None
    } else {
        Some(u32::try_from(d.max_threads).map_err(e("compute max threads"))?)
    };
    out.threadgroup_size_is_multiple_of_execution_width = d.multiple_of_execution_width != 0;
    out.support_indirect_command_buffers = d.indirect != 0;
    out.max_call_stack_depth = u32::try_from(d.max_call_stack_depth).map_err(e("compute stack depth"))?;
    out.support_adding_binary_functions = d.adding_binary_functions != 0;
    if d.stage_inputs != 0 {
        out.stage_input = Some(stage_input(d)?);
    }
    for (index, &mutability) in d.mutability.iter().enumerate() {
        if mutability != 0 {
            out.buffers.insert(
                index as u32,
                raw::<metal2vulkan::metal::Mutability>(mutability, "buffer mutability")?,
            );
        }
    }
    Ok(out)
}

#[repr(C)]
pub struct NmbColorAttachment {
    pub format: u64,
    pub write_mask: u64,
    pub blending: u64,
    pub source_rgb: u64,
    pub destination_rgb: u64,
    pub rgb_operation: u64,
    pub source_alpha: u64,
    pub destination_alpha: u64,
    pub alpha_operation: u64,
}

#[repr(C)]
pub struct NmbVertexAttribute {
    pub format: u64,
    pub offset: u64,
    pub buffer: u64,
}

#[repr(C)]
pub struct NmbVertexLayout {
    pub stride: u64,
    pub step_function: u64,
    pub step_rate: u64,
}

#[repr(C)]
pub struct NmbRenderPipelineDescriptorOf<F> {
    pub vertex: *mut F,
    pub fragment: *mut F,
    pub colors: [NmbColorAttachment; 8],
    pub depth_format: u64,
    pub stencil_format: u64,
    pub samples: u64,
    pub topology: u64,
    pub rasterization: u64,
    pub vertex_inputs: u64,
    pub attributes: [NmbVertexAttribute; 31],
    pub layouts: [NmbVertexLayout; 31],
    pub vertex_linked: *const *mut F,
    pub vertex_linked_count: u64,
    pub vertex_private_linked: *const *mut F,
    pub vertex_private_count: u64,
    pub fragment_linked: *const *mut F,
    pub fragment_linked_count: u64,
    pub fragment_private_linked: *const *mut F,
    pub fragment_private_count: u64,
    pub tessellation: u64,
    pub max_tessellation_factor: u64,
    pub partition_mode: u64,
    pub factor_format: u64,
    pub factor_step_function: u64,
    pub winding: u64,
    pub control_point_index_type: u64,
    pub factor_scale_enabled: u64,
    pub max_amplification: u64,
}

pub fn optional_format(value: u64) -> Result<Option<PixelFormat>, String> {
    if value == 0 {
        Ok(None)
    } else {
        raw::<PixelFormat>(value, "pixel format").map(Some)
    }
}

/// A stage's MTLLinkedFunctions: its functions and privateFunctions.
pub fn stage_linked<F: DescriptorFunction>(
    functions: *const *mut F,
    count: u64,
    private: *const *mut F,
    private_count: u64,
) -> Result<LinkedFunctions, String> {
    let mut linked = LinkedFunctions::new();
    linked.functions = linked_functions(functions, count)?;
    linked.private_functions = linked_functions(private, private_count)?;
    Ok(linked)
}

pub fn render_descriptor<F: DescriptorFunction>(
    descriptor: *const NmbRenderPipelineDescriptorOf<F>,
) -> Result<RenderPipelineDescriptor, String> {
    let d = get(descriptor)?;
    let vertex_linked = stage_linked(
        d.vertex_linked,
        d.vertex_linked_count,
        d.vertex_private_linked,
        d.vertex_private_count,
    )?;
    let mut out = RenderPipelineDescriptor::new(StageSource::Function(
        get(d.vertex)?.stage_function().with_linked(vertex_linked),
    ));
    if !d.fragment.is_null() {
        let fragment_linked = stage_linked(
            d.fragment_linked,
            d.fragment_linked_count,
            d.fragment_private_linked,
            d.fragment_private_count,
        )?;
        out.fragment = Some(StageSource::Function(
            get(d.fragment)?.stage_function().with_linked(fragment_linked),
        ));
    }
    for (slot, c) in out.color_attachments.iter_mut().zip(d.colors.iter()) {
        if c.format == 0 {
            continue;
        }
        let mut a = ColorAttachmentDescriptor::new(raw::<PixelFormat>(c.format, "color format")?);
        a.write_mask = raw::<ColorWriteMask>(c.write_mask, "write mask")?;
        a.blending_enabled = c.blending != 0;
        a.source_rgb_blend_factor = raw::<BlendFactor>(c.source_rgb, "blend factor")?;
        a.destination_rgb_blend_factor = raw::<BlendFactor>(c.destination_rgb, "blend factor")?;
        a.rgb_blend_operation = raw::<BlendOperation>(c.rgb_operation, "blend operation")?;
        a.source_alpha_blend_factor = raw::<BlendFactor>(c.source_alpha, "blend factor")?;
        a.destination_alpha_blend_factor = raw::<BlendFactor>(c.destination_alpha, "blend factor")?;
        a.alpha_blend_operation = raw::<BlendOperation>(c.alpha_operation, "blend operation")?;
        *slot = Some(a);
    }
    out.depth_format = optional_format(d.depth_format)?;
    out.stencil_format = optional_format(d.stencil_format)?;
    out.raster_sample_count = d.samples.max(1) as u32;
    out.input_primitive_topology = raw::<PrimitiveTopologyClass>(d.topology, "topology")?;
    out.rasterization_enabled = d.rasterization != 0;
    if d.vertex_inputs != 0 {
        let mut inputs = VertexDescriptor::new();
        for (i, a) in d.attributes.iter().enumerate() {
            if a.format == 0 {
                continue;
            }
            inputs.attributes.insert(
                i as u32,
                VertexAttributeDescriptor::new(
                    raw::<VertexFormat>(a.format, "vertex format")?,
                    a.offset as u32,
                    a.buffer as u32,
                ),
            );
            if let Some(l) = d.layouts.get(a.buffer as usize) {
                // MTLBufferLayoutStrideDynamic (NSUIntegerMax): the stride each bind sets
                let stride = (l.stride != u64::MAX).then_some(l.stride as u32);
                let mut layout = VertexBufferLayoutDescriptor::new(stride);
                layout.step_function = raw::<VertexStepFunction>(l.step_function, "step function")?;
                layout.step_rate = l.step_rate as u32;
                inputs.layouts.insert(a.buffer as u32, layout);
            }
        }
        out.vertex_descriptor = Some(inputs);
    }
    if d.tessellation != 0 {
        let mut t = TessellationDescriptor::default();
        t.max_tessellation_factor = d.max_tessellation_factor as u32;
        t.partition_mode = raw::<TessellationPartitionMode>(d.partition_mode, "partition mode")?;
        t.factor_format = raw::<TessellationFactorFormat>(d.factor_format, "factor format")?;
        t.factor_step_function = raw::<TessellationFactorStepFunction>(d.factor_step_function, "factor step")?;
        t.output_winding_order = raw::<Winding>(d.winding, "winding")?;
        t.control_point_index_type =
            raw::<TessellationControlPointIndexType>(d.control_point_index_type, "control point index type")?;
        t.factor_scale_enabled = d.factor_scale_enabled != 0;
        out.tessellation = Some(t);
    }
    out.max_vertex_amplification_count = d.max_amplification.max(1) as u32;
    Ok(out)
}

#[repr(C)]
pub struct NmbSamplerDescriptor {
    pub min_filter: u64,
    pub mag_filter: u64,
    pub mip_filter: u64,
    pub s_address: u64,
    pub t_address: u64,
    pub r_address: u64,
    pub border_color: u64,
    pub normalized: u64,
    pub max_anisotropy: u64,
    pub compare: u64,
    pub lod_average: u64,
    pub lod_min: f32,
    pub lod_max: f32,
}

pub fn sampler_descriptor(d: &NmbSamplerDescriptor) -> Result<SamplerDescriptor, String> {
    let mut out = SamplerDescriptor::new();
    out.min_filter = raw::<SamplerMinMagFilter>(d.min_filter, "min filter")?;
    out.mag_filter = raw::<SamplerMinMagFilter>(d.mag_filter, "mag filter")?;
    out.mip_filter = raw::<SamplerMipFilter>(d.mip_filter, "mip filter")?;
    out.s_address_mode = raw::<SamplerAddressMode>(d.s_address, "address mode")?;
    out.t_address_mode = raw::<SamplerAddressMode>(d.t_address, "address mode")?;
    out.r_address_mode = raw::<SamplerAddressMode>(d.r_address, "address mode")?;
    out.border_color = raw::<SamplerBorderColor>(d.border_color, "border color")?;
    out.normalized_coordinates = d.normalized != 0;
    out.max_anisotropy = d.max_anisotropy.max(1) as u32;
    out.compare_function = raw::<CompareFunction>(d.compare, "compare function")?;
    out.lod_average = d.lod_average != 0;
    out.lod_min_clamp = d.lod_min;
    out.lod_max_clamp = d.lod_max;
    Ok(out)
}
