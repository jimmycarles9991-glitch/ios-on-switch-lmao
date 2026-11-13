// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile of a render state (NativeMetalDirect.h): what m2v's translator makes of the
//! state for each of the pack generator's probe draws (drivers/TegraGM20BMetal/raw/pack/probe.mm), with no pipeline
//! state, command buffer or capture. Only the translator's public API is used. A runtime device lends its compile
//! device and its Vulkan device, on which the variants' layouts are made too; the offline compile (the pack
//! generator's, and the system shader compiler's compile context: NativeMetalCompile.rs) runs on the compile profile's
//! devices with the layouts as data alone.

use crate::objects::{
    DescriptorFunction, NmbDevice, NmbRenderPipelineDescriptor, NmbRenderPipelineDescriptorOf, e, get, guard, raw,
    render_descriptor,
};
use crate::offline::{Devices, NmbOfflineRenderPipelineDescriptor, offline_devices};
use ash::vk as avk;
use metal2vulkan::Budget;
use metal2vulkan::bind::{
    BindState, BindingPlan, BoundBuffer, BoundSampler, BoundTexture, DescriptorBytes, DescriptorRange, DrawCall,
    DrawCommands, FieldLocation, FragmentTests, ImageSource, NeutralResource, PushHole, SamplerDescriptor,
    SamplerSource, StageTable, TextureFacts, VertexAmplification, ViewMapping, WriteSource, heap_class_code,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::device::{DeviceDescription, VkExtension, VkFeature};
use metal2vulkan::error::{BindError, Retry};
use metal2vulkan::metal::{
    PixelFormat, SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter, TextureType,
    TextureUsage,
};
use metal2vulkan::pipeline::{
    BindingSource, GraphicsState, PipelineLayout, PushField, RenderPipeline, RenderPipelineDescriptor, RuntimeFact,
    SampledType, SlotClass, SlotInterface, SlotRef, Stage, StageSource, StandInView, StorageAttachmentView,
    SyntheticBinding, TextureSlot, VertexStride,
};
use std::ffi::{CString, c_char, c_void};
use std::sync::{Arc, Mutex};

const PROBE_SLOTS: u32 = 31;
const PROBE_TAGGED: u32 = 16;
const RUN_BYTES: [u32; 4] = [256, 16, 4096, 256];
const BOUND_RUN: usize = 3;
const MAX_WRITES: usize = 64;
pub(crate) const HEAP_ARRAYS: usize = 8;
/// The raw path's texture header pool (raw_heap_tables.h kHeapElements): its heap holds texture header i and sampler header
/// i at element i, so no array needs more elements
pub(crate) const RAW_HEAP_ELEMENTS: u32 = 8192;

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
    pub semantic: *const c_void,
    pub portable_context: *const c_void,
    pub portable: Option<unsafe extern "C" fn(*const c_void, *mut *const u32, *mut usize) -> bool>,
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
    DepthStandIn,
    StencilStandIn,
    TextureTable,
    SamplerTable,
    ArgumentTable,
    TessFactors,
    PatchIndices,
    ControlPointIndices,
}

pub const NMB_DIRECT_IMMUTABLE: u32 = u32::MAX;
/// NmbDirectDraw::array_length_word of a state whose vertex function chooses its slice, compiled for one layer
pub const LAYER_DROPPED: u32 = 0xFF;
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
    pub format: u32,
    pub compare: u32,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbDirectSamplerState {
    pub mag_filter: u32,
    pub min_filter: u32,
    pub mipmap_mode: u32,
    pub address: [u32; 3],
    pub mip_lod_bias: f32,
    pub min_lod: f32,
    pub max_lod: f32,
    pub max_anisotropy: f32,
    pub compare_op: u32,
    pub border_color: u32,
    pub reduction: u32,
    pub border_components: [u32; 4],
    pub anisotropy: bool,
    pub compare: bool,
    pub unnormalized: bool,
}

/// What vk_sampler_state_init reads of `info`, with the reduction mode and border mapping metal2vulkan chains to it
pub(crate) fn sampler_state(
    info: &avk::SamplerCreateInfo<'_>,
    reduction: avk::SamplerReductionMode,
    components: Option<avk::ComponentMapping>,
) -> NmbDirectSamplerState {
    let c = components.unwrap_or_default();
    NmbDirectSamplerState {
        mag_filter: info.mag_filter.as_raw() as u32,
        min_filter: info.min_filter.as_raw() as u32,
        mipmap_mode: info.mipmap_mode.as_raw() as u32,
        address: [info.address_mode_u, info.address_mode_v, info.address_mode_w].map(|a| a.as_raw() as u32),
        mip_lod_bias: info.mip_lod_bias,
        min_lod: info.min_lod,
        max_lod: info.max_lod,
        max_anisotropy: info.max_anisotropy,
        compare_op: if info.compare_enable != 0 {
            info.compare_op.as_raw() as u32
        } else {
            0
        },
        border_color: info.border_color.as_raw() as u32,
        reduction: reduction.as_raw() as u32,
        border_components: [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
        anisotropy: info.anisotropy_enable != 0,
        compare: info.compare_enable != 0,
        unnormalized: info.unnormalized_coordinates != 0,
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectBinding {
    pub binding: u32,
    pub ty: i32,
    pub count: u32,
    pub flags: u32,
    pub immutable: bool,
    pub sampler: NmbDirectSamplerState,
}

#[repr(C)]
pub struct NmbDirectSet {
    pub bindings: *const NmbDirectBinding,
    pub binding_count: u32,
    pub flags: u32,
}

#[repr(C)]
pub struct NmbDirectPushRange {
    pub stages: u32,
    pub offset: u32,
    pub size: u32,
}

#[repr(C)]
pub struct NmbDirectLayout {
    pub sets: *const NmbDirectSet,
    pub set_count: u32,
    pub push: *const NmbDirectPushRange,
    pub push_count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectDraw {
    pub set0: u64,
    pub set0_bindings: *const NmbDirectBinding,
    pub set0_binding_count: u32,
    pub writes: *const NmbDirectWrite,
    pub write_count: u32,
    pub push_mask: u32,
    pub push: [u32; 32],
    pub blend_color_word: u32,
    pub half_blend_color_word: u32,
    pub depth_test_word: u32,
    pub stencil_test_word: u32,
    pub push_holes: u32,
    pub root_table_word: [u8; 2],
    pub status_word: [u8; 2],
    pub other_holes: u32,
    pub root_slots: [u32; 2],
    pub set1: u64,
    pub heap_arrays: u32,
    pub heap_class: [u32; HEAP_ARRAYS],
    pub vertex_slot: u32,
    pub vertex_stride: u32,
    pub vertex_stride_dynamic: u32,
    pub pulled_strides: [[u32; 2]; 4],
    pub amplification_max: u32,
    pub amplification_id_word: u32,
    pub amplification_count_word: u32,
    pub amplification_layer_word: u32,
    pub amplification_viewport_word: u32,
    pub array_length_word: u32,
    pub viewport_count_word: u32,
    pub pulled_attributes: u32,
    pub variant: *const c_char,
    pub spill: [*const u32; 2],
    pub spill_words: [u32; 2],
    pub residual_word: [[u8; 32]; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectCubicSampler {
    pub slot: u32,
    pub s_address: u32,
    pub t_address: u32,
    pub r_address: u32,
    pub border_color: u32,
    pub normalized: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectTextureFormat {
    pub slot: u32,
    pub format: u64,
}

#[repr(C)]
pub struct NmbDirectBinds {
    pub cubic: *const NmbDirectCubicSampler,
    pub cubic_count: u32,
    pub textures: *const NmbDirectTextureFormat,
    pub texture_count: u32,
    pub fragment_tests: u32,
    pub bind_stride: u32,
    pub two_plane: bool,
}

/// What the probe binds besides its tagged textures and samplers: cubic sampler states, and other textures' formats, by
/// slot; and whether the state is compiled for RGB10A8_2P_XR10 attachments held in their two planes
#[derive(Default)]
struct Binds {
    cubic: Vec<(u32, SamplerDescriptor)>,
    textures: Vec<(u32, PixelFormat)>,
    fragment_tests: u32,
    bind_stride: Option<u32>,
    two_plane: bool,
}

impl Binds {
    /// # Safety
    /// `binds` is null or valid, its arrays of their counts
    unsafe fn of(binds: *const NmbDirectBinds) -> Result<Binds, String> {
        let mut out = Binds::default();
        // SAFETY: the caller's contract.
        let Some(b) = (unsafe { binds.as_ref() }) else {
            return Ok(out);
        };
        if b.fragment_tests > 4 {
            return Err("invalid native fragment-test mode".into());
        }
        out.fragment_tests = b.fragment_tests;
        out.bind_stride = (b.bind_stride != 0).then_some(b.bind_stride);
        out.two_plane = b.two_plane;
        let cubic: &[NmbDirectCubicSampler] = if b.cubic.is_null() {
            &[]
        } else {
            // SAFETY: `cubic_count` records at `cubic`.
            unsafe { std::slice::from_raw_parts(b.cubic, b.cubic_count as usize) }
        };
        for c in cubic {
            let mut d = SamplerDescriptor::new();
            d.min_filter = SamplerMinMagFilter::Cubic;
            d.mag_filter = SamplerMinMagFilter::Cubic;
            d.s_address_mode = raw::<SamplerAddressMode>(c.s_address.into(), "address mode")?;
            d.t_address_mode = raw::<SamplerAddressMode>(c.t_address.into(), "address mode")?;
            d.r_address_mode = raw::<SamplerAddressMode>(c.r_address.into(), "address mode")?;
            d.border_color = raw::<SamplerBorderColor>(c.border_color.into(), "border color")?;
            d.normalized_coordinates = c.normalized;
            out.cubic.push((c.slot, d));
        }
        let textures: &[NmbDirectTextureFormat] = if b.textures.is_null() {
            &[]
        } else {
            // SAFETY: `texture_count` records at `textures`.
            unsafe { std::slice::from_raw_parts(b.textures, b.texture_count as usize) }
        };
        for t in textures {
            out.textures
                .push((t.slot, raw::<PixelFormat>(t.format, "texture format")?));
        }
        Ok(out)
    }
}

#[repr(C)]
#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub struct NmbDirectStorage {
    pub mask: u32,
    pub writes: u32,
    pub sample_planes: u32,
    pub samples: u32,
    pub formats: [u32; 8],
    pub fragment_tests: u32,
    pub depth_format: u32,
    pub stencil_format: u32,
    pub test_samples: u32,
    pub test_planes: u32,
    pub blend_mask: u32,
    pub blend_model: u32,
}

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
    for attachment in &graphics.storage_attachments {
        let index = attachment.color_index as usize;
        if index >= storage.formats.len() || storage.mask & (1 << index) != 0 {
            return Err("storage form: invalid color attachment index".into());
        }
        storage.mask |= 1 << index;
        // A write the pipeline's mask leaves no channel of stores nothing: the colour is only read, so its draws need
        // no more than a barrier (describeReads' rung, raw_storage_form.h fetchesOnlyUnwritten)
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

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct NmbDirectTess {
    pub parameters: u32,
    pub control_word: u32,
    pub control_points: u32,
    pub index_bytes: u32,
}

#[repr(C)]
pub struct NmbDirectState {
    pub device: u64,
    pub layout: u64,
    pub layout_data: *const NmbDirectLayout,
    pub stage_count: u32,
    pub stages: [NmbDirectStage; 4],
    pub tess: NmbDirectTess,
    pub sample_shading: bool,
    pub storage_form: bool,
    pub storage: NmbDirectStorage,
    pub blended: u32,
    pub draws: [NmbDirectDraw; 4],
    pub two_plane: bool,
}

/// A translation's failure as the caller's text; one the same request may get past (its deadline, the host's memory) starts
/// "retry: ", which the caller does not keep as the state's refusal
pub(crate) fn compile_error(what: &'static str) -> impl FnOnce(metal2vulkan::error::Error) -> String {
    move |error| match error.retry() {
        Retry::Later => format!("retry: {what}: {error:?}"),
        _ => format!("{what}: {error:?}"),
    }
}

/// The compile device of the probe's barrier form: the runtime's enabled description without the pixel interlock, as the
/// pack's barrier probe ran NVK with it hidden (NVK_DEBUG=no_fsi). One per process, made at its first ask.
static BARRIER: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);
static ORDERED: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);
static COMPUTE: Mutex<Option<([u8; 32], Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);
type Planed = Vec<([u8; 32], Arc<metal2vulkan::device::Device>)>;
static TWO_PLANE: Mutex<Planed> = Mutex::new(Vec::new());

/// `compile` as a profile that holds each RGB10A8_2P_XR10 attachment in its two planes, one per compile device
fn two_plane_device(compile: &metal2vulkan::device::Device) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = TWO_PLANE.lock().map_err(|_| "two-plane compile device lock")?;
    let fingerprint = compile.profile_fingerprint();
    if let Some((_, device)) = kept.iter().find(|(of, _)| *of == fingerprint) {
        return Ok(device.clone());
    }
    let device = Arc::new(compile.with_two_plane_extended_range());
    kept.push((fingerprint, device.clone()));
    Ok(device)
}

pub(crate) fn native_compute_device(nmb: &NmbDevice) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = COMPUTE.lock().map_err(|_| "native compute device lock")?;
    let compile = nmb.device().compile_device();
    let fingerprint = compile.profile_fingerprint();
    if let Some((_, device)) = kept.as_ref().filter(|(of, _)| *of == fingerprint) {
        return Ok(device.clone());
    }
    let device = Arc::new(
        compile
            .with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53)
            .with_heap_elements(RAW_HEAP_ELEMENTS),
    );
    *kept = Some((fingerprint, device.clone()));
    Ok(device)
}

pub(crate) fn specialized_device(nmb: &NmbDevice, barrier: bool) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = if barrier { &BARRIER } else { &ORDERED }
        .lock()
        .map_err(|_| "native compile device lock")?;
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
    let mut policy = *runtime.compile_device().policy();
    let same = metal2vulkan::device::Device::new(enabled.clone(), policy).map_err(e("compile device"))?;
    if same.profile_fingerprint() != runtime.compile_device().profile_fingerprint() {
        return Err("the runtime's compile device is not its enabled description's".into());
    }
    if barrier {
        enabled.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderPixelInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    } else {
        policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::RequireOrdered;
        // The raw stage compiler explicitly interpolates FragCoord.z at the pixel center.
        // This profile uses that contract for sample-depth correction; the runtime profile stays unchanged.
    }
    let device = if barrier {
        metal2vulkan::device::Device::new(enabled, policy)
    } else {
        metal2vulkan::device::Device::new_with_center_fragment_depth(enabled, policy)
    };
    let device = Arc::new(
        device
            .map_err(e("native compile device"))?
            .with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53)
            .with_heap_elements(RAW_HEAP_ELEMENTS),
    );
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

/// probe.mm's sampler at slot `s` (of the fragment stage, or a vertex one): linear filters when s&1, a repeating U when
/// s&2, max LOD 1 + s/2
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

/// The texture slots the probe binds by their declarations: each the state's functions declare at the vertex stage or at
/// a fragment slot past the tagged ones, and each tagged fragment one declared other than a single-sample `texture2d` of
/// floats. A texture of the declared type (Core Animation's colour cubes a 3D one, a `texture2d_ms` or a `depth2d_ms` a
/// multisampled one), sampled type and comparison is bound there, so that each gets its own write at its own Metal index
/// where an unbound or mistyped slot would get the declaration's neutral
fn declared_slots(pipeline: &RenderPipeline) -> Vec<(Stage, u32, TextureSlot)> {
    pipeline
        .interface
        .slots
        .iter()
        .filter_map(|(slot, interface)| match interface {
            SlotInterface::Texture(t)
                if slot.class == SlotClass::Texture
                    && (slot.stage == Stage::Vertex
                        || (slot.stage == Stage::Fragment
                            && (slot.index >= PROBE_TAGGED
                                || t.texture_type != TextureType::Type2D
                                || t.sampled_type != SampledType::Float
                                || t.depth_compare))) =>
            {
                Some((slot.stage, slot.index, *t))
            }
            _ => None,
        })
        .collect()
}

/// The vertex sampler slots the state's functions declare: the probe binds its sampler of each index there, as it does at
/// every fragment one
fn vertex_samplers(pipeline: &RenderPipeline) -> Vec<u32> {
    pipeline
        .interface
        .slots
        .iter()
        .filter_map(|(slot, interface)| match interface {
            SlotInterface::Sampler(_) if slot.stage == Stage::Vertex && slot.class == SlotClass::Sampler => {
                Some(slot.index)
            }
            _ => None,
        })
        .collect()
}

/// What the probe binds at a slot declared `t`: its type (multisampled at the state's samples, else 4), and a format of its
/// sampled type or a depth format for a comparison where the state names none
fn declared_facts(t: &TextureSlot, own: Option<PixelFormat>, format: PixelFormat, samples: u32) -> TextureFacts {
    let pixel_format = own.unwrap_or(match t.sampled_type {
        _ if t.depth_compare => PixelFormat::Depth32Float,
        SampledType::Uint => PixelFormat::RGBA8Uint,
        SampledType::Sint => PixelFormat::RGBA8Sint,
        _ => format,
    });
    let mut facts = TextureFacts::new(t.texture_type, pixel_format);
    if matches!(
        t.texture_type,
        TextureType::Type2DMultisample | TextureType::Type2DMultisampleArray
    ) {
        facts.sample_count = if samples > 1 { samples } else { 4 };
        facts.usage |= TextureUsage::RENDER_TARGET;
    }
    facts
}

/// The probe's address of a buffer at slot `slot` of `stage` (1 vertex, 2 fragment)
fn probe_address(stage: u32, slot: u32) -> u64 {
    0x1_0000_0000u64 + (u64::from(stage) << 24) + (u64::from(slot) << 16)
}

/// The bind state of probe.mm's draw `run`: every buffer slot of both stages bound (inline bytes, or the bound run's
/// buffers at each slot's own offset; a buffer of the run's bytes at a slot of `addressed`, which m2v reaches by its
/// device address; vertex buffers at `binds`' stride), 16 textures of `format` and `samples` and 16 samplers at the
/// fragment stage, a texture of its declaration at each of `declared` and a sampler at each of `vertex_samplers`, `binds`'
/// in their places
#[allow(clippy::too_many_arguments)]
fn probe_state(
    run: usize,
    format: PixelFormat,
    samples: u32,
    declared: &[(Stage, u32, TextureSlot)],
    vertex_samplers: &[u32],
    binds: &Binds,
    addressed: &[SlotRef],
    patches: bool,
    amplification: u32,
    layers: u32,
) -> BindState {
    let mut state = BindState::default();
    state.render_target_array_length = layers;
    // One draw per amplification, which the raw path draws in order, under mappings that offset the viewport (with `layers`
    // 2, a layered pass): its vertex program writes each amplification's layer and viewport from its offset words
    if amplification > 1 {
        let mappings = (0..amplification).map(|a| ViewMapping::new(1, a)).collect();
        state.vertex_amplification = VertexAmplification::new(amplification, mappings);
        state.vertex_amplification.one_at_a_time = true;
    }
    if patches {
        crate::direct_tess::bind_factors(&mut state);
    }
    state.fragment_tests = match binds.fragment_tests {
        1 => FragmentTests::Pass,
        2 => FragmentTests::Drop,
        3 => FragmentTests::Depth { writes: true },
        4 => FragmentTests::Stencil,
        _ => FragmentTests::Any,
    };
    for stage in [Stage::Vertex, Stage::Fragment] {
        let mut table = StageTable::default();
        let attribute_stride = if stage == Stage::Vertex {
            binds.bind_stride
        } else {
            None
        };
        for slot in 0..PROBE_SLOTS {
            let address = probe_address(stage_number(stage), slot);
            let reached = addressed.iter().any(|a| a.stage == stage && a.index == slot);
            let bound = if run == BOUND_RUN {
                BoundBuffer::Buffer {
                    length: 256 + u64::from(RUN_BYTES[run]),
                    offset: probe_bind_offset(stage_number(stage), slot),
                    gpu_address: address,
                    device_address: Some(address),
                    attribute_stride,
                }
            } else if reached {
                BoundBuffer::Buffer {
                    length: u64::from(RUN_BYTES[run]),
                    offset: 0,
                    gpu_address: address,
                    device_address: Some(address),
                    attribute_stride,
                }
            } else {
                BoundBuffer::Bytes {
                    len: RUN_BYTES[run],
                    attribute_stride,
                }
            };
            table.buffers.insert(slot, bound);
        }
        let own_format = |t: u32| {
            binds
                .textures
                .iter()
                .find(|(slot, _)| stage == Stage::Fragment && *slot == t)
                .map(|(_, f)| *f)
        };
        for (_, t, d) in declared
            .iter()
            .filter(|(at, t, _)| *at == stage && (stage == Stage::Vertex || *t >= PROBE_TAGGED))
        {
            table.textures.insert(
                *t,
                BoundTexture::Texture {
                    facts: declared_facts(d, own_format(*t), format, samples),
                    resource_id: None,
                },
            );
        }
        if stage == Stage::Vertex {
            for &s in vertex_samplers {
                table.samplers.insert(s, BoundSampler::new(probe_sampler(s)));
            }
        }
        if stage == Stage::Fragment {
            for t in 0..PROBE_TAGGED {
                let own = own_format(t);
                let mut facts = TextureFacts::new(TextureType::Type2D, own.unwrap_or(format));
                if let Some((_, _, d)) = declared.iter().find(|(at, slot, _)| *at == stage && *slot == t) {
                    facts = declared_facts(d, own, format, samples);
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
                let cubic = binds.cubic.iter().find(|(slot, _)| *slot == t).map(|(_, d)| *d);
                table
                    .samplers
                    .insert(t, BoundSampler::new(cubic.unwrap_or_else(|| probe_sampler(t))));
            }
        }
        state.stages.insert(stage, table);
    }
    state
}

/// The MTLPixelFormat of the texture the probe bound for fragment texture write `w` (0: none of its own), which the stage's
/// accesses of a storage texture were compiled for
fn probed_format(state: &BindState, w: &NmbDirectWrite) -> u32 {
    if w.stage != 2 || !matches!(w.source, NmbDirectSource::Texture | NmbDirectSource::Combined) {
        return 0;
    }
    match state
        .stages
        .get(&Stage::Fragment)
        .and_then(|table| table.textures.get(&w.slot))
    {
        Some(BoundTexture::Texture { facts, .. }) => facts.pixel_format.as_raw(),
        _ => 0,
    }
}

pub(crate) fn slot_parts(slot: &SlotRef) -> (u32, u32) {
    (stage_number(slot.stage), slot.index)
}

fn neutral_view(neutral: &NeutralResource) -> i32 {
    match neutral {
        NeutralResource::Image { view_type, .. } => avk::ImageViewType::from(*view_type).as_raw(),
        _ => -1,
    }
}

pub(crate) fn write_of(w: &metal2vulkan::bind::DescriptorWrite, ranges: &[DescriptorRange]) -> NmbDirectWrite {
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
        format: 0,
        compare: 0,
    };
    let sampler_of = |s: &SamplerSource| match s {
        SamplerSource::Slot { slot, .. } => slot.index,
        SamplerSource::Immutable => NMB_DIRECT_IMMUTABLE,
        _ => NMB_DIRECT_NEUTRAL,
    };
    let compares = |s: &SamplerSource| match s {
        SamplerSource::Slot { compare, .. } => u32::from(*compare),
        SamplerSource::Neutral(NeutralResource::Sampler { compare }) => u32::from(*compare),
        _ => 0,
    };
    match &w.source {
        WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => {
            out.source = if matches!(w.source, WriteSource::Buffer { .. }) {
                NmbDirectSource::Buffer
            } else {
                NmbDirectSource::Bytes
            };
            (out.stage, out.slot) = slot_parts(slot);
            out.range = ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        WriteSource::Texture { slot, view } => {
            out.source = NmbDirectSource::Texture;
            (out.stage, out.slot) = slot_parts(slot);
            out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
        }
        WriteSource::Sampler { slot, compare, .. } => {
            out.source = NmbDirectSource::Sampler;
            (out.stage, out.slot) = slot_parts(slot);
            out.sampler = slot.index;
            out.compare = u32::from(*compare);
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
            out.compare = compares(sampler);
        }
        WriteSource::Attachment { color_index } => {
            out.source = NmbDirectSource::Attachment;
            out.slot = *color_index;
        }
        WriteSource::TileAttachment { color_index, view, .. } => {
            out.source = NmbDirectSource::StorageAttachment;
            out.stage = 2;
            out.slot = *color_index;
            out.view_type = match view {
                StorageAttachmentView::Attachment => avk::ImageViewType::TYPE_2D.as_raw(),
                StorageAttachmentView::SamplePlanes { .. } => avk::ImageViewType::TYPE_2D_ARRAY.as_raw(),
            };
        }
        WriteSource::DepthStandIn | WriteSource::StencilStandIn => {
            out.source = if matches!(w.source, WriteSource::DepthStandIn) {
                NmbDirectSource::DepthStandIn
            } else {
                NmbDirectSource::StencilStandIn
            };
            out.stage = 2;
            out.view_type = avk::ImageViewType::TYPE_2D.as_raw();
        }
        WriteSource::Neutral(neutral) => {
            out.source = NmbDirectSource::Neutral;
            out.view_type = neutral_view(neutral);
        }
        WriteSource::ColorWriteTable => {
            out.source = NmbDirectSource::ColorTable;
            out.stage = 2;
        }
        WriteSource::HandleTable { stage, bytes_len } | WriteSource::ArgumentTable { stage, bytes_len } => {
            out.source = if matches!(w.source, WriteSource::ArgumentTable { .. }) {
                NmbDirectSource::ArgumentTable
            } else {
                NmbDirectSource::TextureTable
            };
            out.stage = stage_number(*stage);
            out.range = u64::from(*bytes_len);
        }
        WriteSource::TessellationFactors | WriteSource::PatchIndices { .. } | WriteSource::ControlPointIndices => {
            out.source = match w.source {
                WriteSource::TessellationFactors => NmbDirectSource::TessFactors,
                WriteSource::PatchIndices { .. } => NmbDirectSource::PatchIndices,
                _ => NmbDirectSource::ControlPointIndices,
            };
            out.stage = 1;
            out.range = ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        _ => {}
    }
    out
}

/// A handle table's write as its binding's synthetic kind names it, not by its size or place: the texture table or the
/// sampler table
fn table_of(mut write: NmbDirectWrite, layout: &PipelineLayout) -> NmbDirectWrite {
    if write.source != NmbDirectSource::TextureTable {
        return write;
    }
    let source = layout
        .sets
        .iter()
        .filter(|s| s.set == write.set)
        .flat_map(|s| &s.bindings)
        .find(|b| b.binding == write.binding)
        .map(|b| &b.source);
    write.source = match source {
        Some(BindingSource::Synthetic {
            binding: SyntheticBinding::SamplerTable,
            ..
        }) => NmbDirectSource::SamplerTable,
        Some(BindingSource::Synthetic {
            binding: SyntheticBinding::HandleTable,
            ..
        }) => NmbDirectSource::TextureTable,
        _ => NmbDirectSource::Other,
    };
    write
}

/// The words instantiation gave `stage`'s argument table (m2v's push fields past the stage's push window), empty where its
/// placement spilled none
pub(crate) fn spill_table(plan: &BindingPlan, bytes: &[DescriptorBytes], stage: Stage) -> Vec<u32> {
    let Some(write) = plan
        .descriptors
        .iter()
        .find(|w| matches!(w.source, WriteSource::ArgumentTable { stage: s, .. } if s == stage))
    else {
        return Vec::new();
    };
    bytes
        .iter()
        .find(|b| b.set == write.set && b.binding == write.binding)
        .map_or_else(Vec::new, |b| {
            b.bytes
                .chunks(4)
                .map(|w| {
                    w.iter()
                        .enumerate()
                        .fold(0u32, |v, (k, byte)| v | u32::from(*byte) << (8 * k))
                })
                .collect()
        })
}

/// By buffer slot, the record's word + 1 of each SlotBindOffset residual of `stage` as the plan places it: a root push
/// word (1 to 32), or the stage's argument table word + 33 (33 to 63). The probe's codes cannot name a storage residual
/// whose slot shares its code with another.
pub(crate) fn residual_words(plan: &BindingPlan, stage: Stage) -> [u8; 32] {
    let mut out = [0u8; 32];
    for field in plan.push.fields.iter().filter(|f| f.stage == stage) {
        let PushField::SlotBindOffset(slot) = field.field else {
            continue;
        };
        if slot.class != SlotClass::Buffer || slot.index >= 32 {
            continue;
        }
        let word = match field.location {
            FieldLocation::PushWindow { offset } if offset / 4 < 32 => offset / 4 + 1,
            FieldLocation::ArgumentTable { offset } if offset / 4 < 31 => offset / 4 + 33,
            _ => continue,
        };
        out[slot.index as usize] = word as u8;
    }
    out
}

/// Set 0's samplers that take no write: a heap image's constexpr sampler (BindingSource::HeapSampler's immutable one), whose
/// descriptor NVK writes into the set from its layout
fn immutable_samplers(layout: &PipelineLayout, written: &[NmbDirectWrite]) -> Vec<NmbDirectWrite> {
    let mut out = Vec::new();
    for b in layout.sets.iter().filter(|s| s.set == 0).flat_map(|s| &s.bindings) {
        let sampler = avk::DescriptorType::from(b.ty) == avk::DescriptorType::SAMPLER;
        if !sampler || b.immutable_sampler.is_none() || written.iter().any(|w| w.binding == b.binding) {
            continue;
        }
        for element in 0..b.count {
            out.push(NmbDirectWrite {
                set: 0,
                binding: b.binding,
                element,
                ty: avk::DescriptorType::SAMPLER.as_raw(),
                source: NmbDirectSource::Sampler,
                stage: 2,
                slot: 0,
                sampler: NMB_DIRECT_IMMUTABLE,
                range: 0,
                view_type: -1,
                format: 0,
                compare: 0,
            });
        }
    }
    out
}

/// The stages of a hole's flags as raw_direct.cpp numbers them: bit 0 vertex, bit 1 fragment
fn hole_stages(stages: metal2vulkan::vk::ShaderStageFlags) -> u32 {
    use metal2vulkan::vk::ShaderStageFlags as S;
    u32::from(stages.contains(S::VERTEX)) | u32::from(stages.contains(S::FRAGMENT)) << 1
}

/// The holes of `commands` the raw path fills into `draw` (each stage's root-table and ExecutionStatus address, as push word
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

/// The variant's set-1 heap: its set layout on the runtime's device (0 with none) and each array's class word, by
/// binding
fn heap_of(pipeline: &RenderPipeline, layout: &Layout<'_>, draw: &mut NmbDirectDraw) {
    let Some((at, set)) = pipeline.layout.sets.iter().enumerate().find(|(_, s)| s.set == 1) else {
        return;
    };
    draw.set1 = layout.sets.get(at).map_or(0, |s| avk::Handle::as_raw(*s));
    draw.heap_arrays = set.bindings.len() as u32;
    for b in &set.bindings {
        let code = match &b.source {
            BindingSource::Heap(class) => heap_class_code(class).unwrap_or(0),
            _ => 0,
        };
        if let Some(slot) = draw.heap_class.get_mut(b.binding as usize) {
            *slot = code;
        }
    }
}

/// A variant's layout as data, and as the runtime makes it on its device where there is one (pipeline/objects.rs
/// create_set_layout); with no device its handles are 0
pub(crate) struct Layout<'d> {
    device: Option<&'d ash::Device>,
    samplers: Vec<avk::Sampler>,
    sets: Vec<avk::DescriptorSetLayout>,
    pub(crate) pipeline: avk::PipelineLayout,
    /// Set 0's bindings, as data
    pub(crate) set0_bindings: Vec<NmbDirectBinding>,
    set_bindings: Vec<Vec<NmbDirectBinding>>,
    set_records: Vec<NmbDirectSet>,
    push_ranges: Vec<NmbDirectPushRange>,
    pub(crate) data: NmbDirectLayout,
}

impl<'d> Layout<'d> {
    pub(crate) fn new(device: Option<&'d ash::Device>, layout: &PipelineLayout) -> Result<Layout<'d>, String> {
        let mut made = Layout {
            device,
            samplers: Vec::new(),
            sets: Vec::new(),
            pipeline: avk::PipelineLayout::null(),
            set0_bindings: Vec::new(),
            set_bindings: Vec::new(),
            set_records: Vec::new(),
            push_ranges: Vec::new(),
            data: NmbDirectLayout {
                sets: std::ptr::null(),
                set_count: 0,
                push: std::ptr::null(),
                push_count: 0,
            },
        };
        for (set_index, set) in layout.sets.iter().enumerate() {
            let mut immutable: Vec<Vec<avk::Sampler>> = Vec::with_capacity(set.bindings.len());
            let mut data_bindings = Vec::with_capacity(set.bindings.len());
            for binding in &set.bindings {
                let mut data = NmbDirectBinding {
                    binding: binding.binding,
                    ty: avk::DescriptorType::from(binding.ty).as_raw(),
                    count: binding.count,
                    flags: avk::DescriptorBindingFlags::from(binding.flags).as_raw(),
                    immutable: false,
                    sampler: NmbDirectSamplerState::default(),
                };
                let Some(desc) = binding.immutable_sampler else {
                    immutable.push(Vec::new());
                    data_bindings.push(data);
                    continue;
                };
                let mut reduction = avk::SamplerReductionModeCreateInfo::default();
                let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
                let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
                data.immutable = true;
                data.sampler = sampler_state(
                    &info,
                    desc.reduction.into(),
                    desc.border_components.map(avk::ComponentMapping::from),
                );
                data_bindings.push(data);
                let Some(device) = device else {
                    immutable.push(Vec::new());
                    continue;
                };
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
            if set_index == 0 {
                made.set0_bindings = data_bindings.clone();
            }
            made.set_bindings.push(data_bindings);
            let Some(device) = device else {
                continue;
            };
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
        made.push_ranges = layout
            .push_constants
            .iter()
            .map(|range| NmbDirectPushRange {
                stages: avk::ShaderStageFlags::from(range.stages).as_raw(),
                offset: range.offset,
                size: range.size,
            })
            .collect();
        made.set_records = layout
            .sets
            .iter()
            .zip(&made.set_bindings)
            .map(|(set, bindings)| NmbDirectSet {
                bindings: bindings.as_ptr(),
                binding_count: bindings.len() as u32,
                flags: avk::DescriptorSetLayoutCreateFlags::from(set.flags).as_raw(),
            })
            .collect();
        made.data = NmbDirectLayout {
            sets: made.set_records.as_ptr(),
            set_count: made.set_records.len() as u32,
            push: made.push_ranges.as_ptr(),
            push_count: made.push_ranges.len() as u32,
        };
        let Some(device) = device else {
            return Ok(made);
        };
        let push: Vec<avk::PushConstantRange> = layout
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

    pub(crate) fn set0(&self) -> u64 {
        self.sets.first().map_or(0, |s| avk::Handle::as_raw(*s))
    }

    /// The handle of the `at`th set layout, 0: none
    pub(crate) fn set_at(&self, at: usize) -> u64 {
        self.sets.get(at).map_or(0, |s| avk::Handle::as_raw(*s))
    }
}

impl Drop for Layout<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        // SAFETY: made here, used by no pipeline or set: the stage compile only reads them.
        unsafe {
            device.destroy_pipeline_layout(self.pipeline, None);
            for &set in &self.sets {
                device.destroy_descriptor_set_layout(set, None);
            }
            for &sampler in &self.samplers {
                device.destroy_sampler(sampler, None);
            }
        }
    }
}

/// One probe draw: the variant it runs on (its index among `variants`), its plan, its commands and its bind state
struct Run {
    variant: usize,
    commands: DrawCommands,
    plan: BindingPlan,
    state: BindState,
}

/// Why a probe draw did not settle: a buffer slot m2v reaches by its device address was bound without one (the probe binds
/// it a buffer then), or the translator's refusal
enum Unsettled {
    Address(SlotRef),
    Refused(String),
}

impl From<String> for Unsettled {
    fn from(why: String) -> Unsettled {
        Unsettled::Refused(why)
    }
}

fn settle(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    variants: &mut Vec<(VariantKey, RenderPipeline)>,
    state: BindState,
    draw: &DrawCall,
    budget: &Budget,
) -> Result<Run, Unsettled> {
    let shape = state.shape();
    let plan = match variants[0].1.plan(compile, &shape) {
        Err(BindError::MissingDeviceAddress { slot }) => return Err(Unsettled::Address(slot)),
        plan => plan.map_err(e("plan"))?,
    };
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
        match variants[at].1.plan(compile, &shape) {
            Err(BindError::MissingDeviceAddress { slot }) => return Err(Unsettled::Address(slot)),
            plan => (at, plan.map_err(e("plan variant"))?),
        }
    };
    let commands = plan.instantiate_draw(&state, draw).map_err(e("instantiate"))?;
    Ok(Run {
        variant,
        commands,
        plan,
        state,
    })
}

fn draw_of(
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
        blend_color_word: 0,
        half_blend_color_word: 0,
        depth_test_word: 0,
        stencil_test_word: 0,
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
        pulled_strides: [[0; 2]; 4],
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
    };
    holes_of(&run.commands, &mut draw)?;
    heap_of(pipeline, layout, &mut draw);
    dynamic_words(run, pipeline, &mut draw)?;
    for field in &run.plan.push.fields {
        let (word, bytes) = match field.field {
            // a static root's guest address, which the probe's draws fill with their own
            PushField::SlotGuestBase(_) => {
                draw.other_holes += 1;
                continue;
            }
            PushField::BlendColor => (&mut draw.blend_color_word, 32),
            PushField::HalfBlendColor => (&mut draw.half_blend_color_word, 16),
            PushField::ShaderTestsState => (&mut draw.depth_test_word, 16),
            PushField::ShaderStencilState => (&mut draw.stencil_test_word, 16),
            PushField::RenderTargetArrayLength => (&mut draw.array_length_word, 4),
            PushField::ViewportCount => (&mut draw.viewport_count_word, 4),
            _ => continue,
        };
        // a field past the window is a word of the fragment stage's argument table: raw_spill_table.h numbers it from 32
        let (offset, first, end) = match field.location {
            FieldLocation::PushWindow { offset } => (offset, 0, 128),
            FieldLocation::ArgumentTable { offset } if field.stage == Stage::Fragment => {
                (offset, 128, 4 * spills[1].len() as u32)
            }
            FieldLocation::ArgumentTable { .. } => {
                return Err("a vertex stage's dynamic state in its argument table".into());
            }
            _ => return Err("dynamic state at a push location the raw path does not know".into()),
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
                    return Err("more than four pulled vertex buffers of a dynamic stride".into());
                }
                draw.pulled_strides[pulled] = [layout, word];
                pulled += 1;
            }
            _ => {}
        }
    }
    Ok(())
}

/// The words used by Metal extended-range blending for `setBlendColorRed:green:blue:alpha:`.
///
/// # Safety
/// `color` and `words` point to four readable floats and eight writable words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn switch_nmb_blend_color_words(color: *const f32, words: *mut u32) {
    // SAFETY: the caller supplies both complete arrays.
    unsafe {
        let color = color.cast::<[f32; 4]>().read();
        words
            .cast::<[u32; 8]>()
            .write(metal2vulkan::bind::extended_blend_color_words(color));
    }
}

/// The pipeline's stages in NmbDirectState's order: vertex and fragment, then a tessellation state's control and evaluation
fn stage_order(pipeline: &RenderPipeline) -> Result<Vec<usize>, String> {
    let wanted: &[avk::ShaderStageFlags] = if pipeline.graphics.tessellation.is_some() {
        &[
            avk::ShaderStageFlags::VERTEX,
            avk::ShaderStageFlags::FRAGMENT,
            avk::ShaderStageFlags::TESSELLATION_CONTROL,
            avk::ShaderStageFlags::TESSELLATION_EVALUATION,
        ]
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

/// `texture_format`, `texture_samples`: the probe's tagged textures' MTLPixelFormat (0: BGRA8Unorm) and sample count, as
/// probe.mm binds them for the state; `binds` (null: none): what it binds besides.
///
/// # Safety
/// `descriptor` is a valid record; `binds` is null or valid; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_direct(
    device: *mut NmbDevice,
    descriptor: *const NmbRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        // SAFETY: the caller's contract.
        let binds = unsafe { Binds::of(binds) }?;
        let nmb = get(device)?;
        let descriptor = render_descriptor(descriptor)?;
        // Every raw form of a state (default, storage, ordered, barrier) and every compute kernel lowers half on SM53, so
        // a storage form and its barrier twin round alike. m2v's own twin stays on the Vulkan target (no float16 below SM70).
        let compile = if barrier || binds.fragment_tests != 0 {
            specialized_device(nmb, barrier)?
        } else {
            native_compute_device(nmb)?
        };
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { nmb.device().raw() };
        translate(
            &compile,
            &descriptor,
            Some(handles.device),
            texture_format,
            texture_samples,
            &binds,
            each,
            ctx,
        )
    })
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
pub(crate) unsafe fn render_offline<F: DescriptorFunction>(
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

/// The state `descriptor` describes as the translator makes it on `compile` (its two-plane profile where `binds` ask)
/// for the probe's draws, its layouts made on `vulkan` where there is one, handed to `each`
#[allow(clippy::too_many_arguments)]
fn translate(
    compile: &metal2vulkan::device::Device,
    descriptor: &RenderPipelineDescriptor,
    vulkan: Option<&ash::Device>,
    texture_format: u64,
    texture_samples: u32,
    binds: &Binds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> Result<bool, String> {
    let planes = match binds.two_plane {
        true => Some(two_plane_device(compile)?),
        false => None,
    };
    let compile = planes.as_deref().unwrap_or(compile);
    let format = match texture_format {
        0 => PixelFormat::BGRA8Unorm,
        f => raw::<PixelFormat>(f, "texture format")?,
    };
    let amplification = descriptor.max_vertex_amplification_count.max(1);
    let budget = Budget::standard();
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
    // length. One the translation has no layered form of (a fragment function of tile memory, an imageblock, the
    // shader-side stencil or depth test, or an extended-range shadow) is compiled for one layer, its record marked so
    // (LAYER_DROPPED): a layered pass draws it elsewhere.
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
    // attachment_view_in), which its storage images address at each primitive's layer
    let writes: Vec<Vec<NmbDirectWrite>> = runs
        .iter()
        .map(|run| {
            let layout = &variants[run.variant].1.layout;
            let mut writes: Vec<NmbDirectWrite> = run
                .plan
                .descriptors
                .iter()
                .filter(|w| w.set == 0)
                .take(MAX_WRITES)
                .map(|w| {
                    let mut out = table_of(write_of(w, &run.commands.descriptor_ranges), layout);
                    if run.plan.variant.layered_storage && out.source == NmbDirectSource::StorageAttachment {
                        out.view_type = avk::ImageViewType::TYPE_2D_ARRAY.as_raw();
                    }
                    out.format = probed_format(&run.state, &out);
                    out
                })
                .collect();
            writes.extend(immutable_samplers(layout, &writes));
            writes
        })
        .collect();
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
    if storage.mask != 0 {
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
        two_plane: binds.two_plane,
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
