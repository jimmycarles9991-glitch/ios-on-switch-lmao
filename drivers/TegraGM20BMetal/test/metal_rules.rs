// SPDX-License-Identifier: MIT
//! The translator's own rules for which heap array of a class holds a texture, a texture's handle-table flags, a
//! sampler's word and which formats are depth or stencil (metal2vulkan bind/heap.rs, bind/heap_code.rs,
//! metal/sampler.rs, metal/format): metal_rules_test holds heap_fit.h, sampler_word.h and raw's linear rows
//! to them.

use metal2vulkan::bind::{TextureFacts, handle_entry_flags, heap_code_holds};
use metal2vulkan::metal::format::is_depth_stencil;
use metal2vulkan::metal::sampler::SamplerWord;
use metal2vulkan::metal::{PixelFormat, TextureSwizzle, TextureSwizzleChannels, TextureType, TextureUsage};
use native_metal_translate::descriptors::{NmbSamplerDescriptor, raw, sampler_descriptor};
use std::mem::{offset_of, size_of};

/// heap_tables.h HeapTextureFacts: a texture's MTLTextureDescriptor
#[repr(C)]
pub struct HeapTextureFacts {
    pub pixel_format: u32,
    pub texture_type: u32,
    pub sample_count: u32,
    pub usage: u32,
    pub array_length: u32,
    pub levels: u32,
    pub swizzle: [u8; 4],
}

fn facts_of(f: &HeapTextureFacts) -> Result<TextureFacts, String> {
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

/// The layouts the caller's structs must have: the sampler descriptor's size and its lod_min's offset, the texture
/// facts' size and their swizzle's offset.
///
/// # Safety
/// `out` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn metal_rules_layouts(out: *mut [usize; 4]) {
    let layouts = [
        size_of::<NmbSamplerDescriptor>(),
        offset_of!(NmbSamplerDescriptor, lod_min),
        size_of::<HeapTextureFacts>(),
        offset_of!(HeapTextureFacts, swizzle),
    ];
    // SAFETY: the caller's four words.
    if let Some(out) = unsafe { out.as_mut() } {
        *out = layouts;
    }
}

/// Whether a heap array of class `heap_class` (metal2vulkan heap_class_code) holds a texture of `facts` at its element;
/// false for facts the translator names no texture of.
///
/// # Safety
/// `facts` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn metal_rules_heap_holds(heap_class: u32, facts: *const HeapTextureFacts) -> bool {
    // SAFETY: the caller's contract.
    let Some(f) = (unsafe { facts.as_ref() }) else {
        return false;
    };
    facts_of(f).is_ok_and(|facts| heap_code_holds(heap_class, &facts))
}

/// The flags word of the handle-table entry of a texture of `facts` (metal2vulkan handle_entry_flags), 0 for facts the
/// translator names no texture of.
///
/// # Safety
/// `facts` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn metal_rules_heap_texture_flags(facts: *const HeapTextureFacts) -> u32 {
    // SAFETY: the caller's contract.
    let Some(f) = (unsafe { facts.as_ref() }) else {
        return 0;
    };
    facts_of(f).map_or(0, |facts| handle_entry_flags(&facts))
}

/// The SamplerWord of a sampler of `descriptor` (the sampler table's flags word), 0 for one the translator names no
/// sampler of.
///
/// # Safety
/// `descriptor` is null or valid.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn metal_rules_sampler_word(descriptor: *const NmbSamplerDescriptor) -> u32 {
    // SAFETY: the caller's contract.
    let Some(d) = (unsafe { descriptor.as_ref() }) else {
        return 0;
    };
    sampler_descriptor(d).map_or(0, |d| SamplerWord::of(&d).0)
}

/// Whether `format` has a depth or stencil aspect; false for a value that names no pixel format.
#[unsafe(no_mangle)]
pub extern "C" fn metal_rules_depth_stencil(format: u32) -> bool {
    raw::<PixelFormat>(format.into(), "pixel format").is_ok_and(is_depth_stencil)
}
