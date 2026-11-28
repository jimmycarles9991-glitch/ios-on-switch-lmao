// SPDX-License-Identifier: MIT

use ash::vk as avk;
use metal2vulkan::pipeline::Stage;
use std::ffi::{c_char, c_void};

pub const HEAP_ARRAYS: usize = 8;
/// pack.h kPackGuestBases, and its fragment stage's bit in guestBaseSlot
pub const GUEST_BASES: usize = 4;
pub const GUEST_BASE_FRAGMENT: u8 = 0x80;
/// The driver's texture header pool (heap_tables.h kHeapElements): its heap holds texture header i and sampler header
/// i at element i, so no array needs more elements
pub const SWITCH_METAL_HEAP_ELEMENTS: u32 = 8192;

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
    pub half_blend_color_word: u32,
    pub depth_test_word: u32,
    pub stencil_test_word: u32,
    pub shadow_stencil_word: u32,
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
    pub pulled_strides: [[u32; 2]; 8],
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
    pub guest_base_word: [u8; GUEST_BASES],
    pub guest_base_slot: [u8; GUEST_BASES],
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
    pub vertex: bool,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct NmbDirectTextureFormat {
    pub slot: u32,
    pub format: u64,
    pub vertex: bool,
}

#[repr(C)]
pub struct NmbDirectBinds {
    pub cubic: *const NmbDirectCubicSampler,
    pub cubic_count: u32,
    pub textures: *const NmbDirectTextureFormat,
    pub texture_count: u32,
    pub fragment_tests: u32,
    pub bind_stride: u32,
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
    pub shadow_format: u32,
    pub shadow_samples: u32,
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
}

pub(crate) fn stage_number(stage: Stage) -> u32 {
    match stage {
        Stage::Vertex => 1,
        Stage::Fragment => 2,
        _ => 0,
    }
}
