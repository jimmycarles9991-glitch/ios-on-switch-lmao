// SPDX-License-Identifier: MIT
//! What the raw path's device-wide heap (drivers/TegraGM20BMetal/raw/runtime/raw_heap_tables.h) asks of m2v: whether a
//! heap array of a class holds a texture, a texture's handle-table flags and a sampler's word, each by m2v's own rule
//! (metal2vulkan bind/heap.rs, bind/heap_code.rs, metal/sampler.rs).

use crate::objects::{NmbSamplerDescriptor, raw, sampler_descriptor};
use metal2vulkan::bind::{TextureFacts, handle_entry_flags, heap_code_holds};
use metal2vulkan::metal::sampler::SamplerWord;
use metal2vulkan::metal::{PixelFormat, TextureSwizzle, TextureSwizzleChannels, TextureType, TextureUsage};

/// raw_heap_tables.h HeapTextureFacts: a texture's MTLTextureDescriptor
#[repr(C)]
pub struct NmbHeapTextureFacts {
    pub pixel_format: u32,
    pub texture_type: u32,
    pub sample_count: u32,
    pub usage: u32,
    pub array_length: u32,
    pub levels: u32,
    pub swizzle: [u8; 4],
}

fn facts_of(f: &NmbHeapTextureFacts) -> Result<TextureFacts, String> {
    let mut facts = TextureFacts::new(
        raw::<TextureType>(f.texture_type.into(), "texture type")?,
        raw::<PixelFormat>(f.pixel_format.into(), "pixel format")?,
    );
    facts.sample_count = f.sample_count.max(1);
    facts.usage = raw::<TextureUsage>(f.usage.into(), "texture usage")?;
    facts.array_length = f.array_length.max(1);
    facts.mipmap_level_count = f.levels.max(1);
    let channel = |c: u8| raw::<TextureSwizzle>(c.into(), "texture swizzle");
    facts.swizzle = TextureSwizzleChannels {
        red: channel(f.swizzle[0])?,
        green: channel(f.swizzle[1])?,
        blue: channel(f.swizzle[2])?,
        alpha: channel(f.swizzle[3])?,
    };
    Ok(facts)
}

/// Whether a heap array of class `heap_class` (metal2vulkan heap_class_code) holds a texture of `facts` at its element; false
/// for facts m2v names no texture of.
///
/// # Safety
/// `facts` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn switch_nmb_heap_holds(heap_class: u32, facts: *const NmbHeapTextureFacts) -> bool {
    // SAFETY: the caller's contract.
    let Some(f) = (unsafe { facts.as_ref() }) else {
        return false;
    };
    facts_of(f).is_ok_and(|facts| heap_code_holds(heap_class, &facts))
}

/// The flags word of the handle-table entry of a texture of `facts` (metal2vulkan handle_entry_flags), 0 for facts m2v names
/// no texture of.
///
/// # Safety
/// `facts` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn switch_nmb_heap_texture_flags(facts: *const NmbHeapTextureFacts) -> u32 {
    // SAFETY: the caller's contract.
    let Some(f) = (unsafe { facts.as_ref() }) else {
        return 0;
    };
    facts_of(f).map_or(0, |facts| handle_entry_flags(&facts))
}

/// The SamplerWord of a sampler of `descriptor` (the sampler table's flags word), 0 for one m2v names no sampler of.
///
/// # Safety
/// `descriptor` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn switch_nmb_sampler_word(descriptor: *const NmbSamplerDescriptor) -> u32 {
    // SAFETY: the caller's contract.
    let Some(d) = (unsafe { descriptor.as_ref() }) else {
        return 0;
    };
    sampler_descriptor(d).map_or(0, |d| SamplerWord::of(&d).0)
}
