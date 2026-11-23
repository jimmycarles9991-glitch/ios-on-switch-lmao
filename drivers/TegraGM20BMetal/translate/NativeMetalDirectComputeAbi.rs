// SPDX-License-Identifier: MIT

use crate::descriptors::NmbSamplerDescriptor;
use crate::direct_abi::{HEAP_ARRAYS, NmbDirectBinding, NmbDirectSamplerState, NmbDirectStage, NmbDirectWrite};

#[repr(C)]
pub struct NmbDirectBoundTexture {
    pub slot: u32,
    pub planes: u32,
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
    pub plane: u32,
    pub sampler: u64,
    pub sampler_state: NmbDirectSamplerState,
    pub has_sampler: bool,
}

#[repr(C)]
pub struct NmbDirectKernel {
    pub device: u64,
    pub layout: u64,
    pub layout_data: *const crate::direct_abi::NmbDirectLayout,
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
