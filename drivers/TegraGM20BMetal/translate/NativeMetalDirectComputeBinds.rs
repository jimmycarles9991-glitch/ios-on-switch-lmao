// SPDX-License-Identifier: MIT

use crate::address_maps;
use crate::descriptors::{e, raw, sampler_descriptor};
use crate::direct_compute_abi::{NmbDirectBoundTexture, NmbDirectDispatch};
use crate::kernel_textures::{TWO_PLANES, kernel_texture};
use metal2vulkan::bind::{BindState, BoundBuffer, BoundSampler, BoundTexture, StageInRegion, StageTable, TextureFacts};
use metal2vulkan::metal::sampler::AddressModeMap;
use metal2vulkan::metal::{PixelFormat, SamplerAddressMode, TextureSwizzleChannels, TextureType, TextureUsage};
use metal2vulkan::pipeline::{BindingSource, PipelineLayout, SamplerRef, Stage, TextureAccess};

const SLOTS: u32 = 31;
const TAG_BYTES: u32 = 4096;
const MOVED_OFFSET: u64 = 256;

// A format the translator drops a write-only slot's writes of (metal2vulkan format.rs `writes_dropped`: a 4:2:0 one)
const WRITES_DROPPED: PixelFormat = PixelFormat::YCBCR84202P;

/// Which of the probe's dispatches a bind state is: 4096 inline bytes at every slot, the same bytes `MOVED_OFFSET` into a
/// buffer of each slot's own, or each of those bound `1 + slot % 15` bytes further (pack/probe_bind.h probeComputeCode)
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum Run {
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

/// Map `map` (address_maps.rs) as the translator's
fn address_modes(map: u8) -> AddressModeMap {
    let edge = SamplerAddressMode::ClampToEdge;
    let mut out = AddressModeMap::IDENTITY;
    if map & address_maps::WRAPPED != 0 {
        for mode in [
            SamplerAddressMode::MirrorClampToEdge,
            SamplerAddressMode::Repeat,
            SamplerAddressMode::MirrorRepeat,
        ] {
            out = out.with(mode, edge);
        }
    }
    if map & address_maps::CLAMPS != 0 {
        for mode in [SamplerAddressMode::ClampToZero, SamplerAddressMode::ClampToBorderColor] {
            out = out.with(mode, edge);
        }
    }
    out
}

/// The map of the texture a dispatch binds at `slot`, 0 for none
fn bound_map(ask: &NmbDirectDispatch, slot: u32) -> u8 {
    counted(ask.textures, ask.texture_count)
        .iter()
        .find(|t| t.slot == slot)
        .and_then(|t| {
            let pf = raw::<PixelFormat>(t.pixel_format, "pixel format").ok()?;
            Some(kernel_texture(pf, t.planes).map)
        })
        .unwrap_or(0)
}

/// `state` with each runtime sampler the kernel samples a converted texture through mapped by that texture's map, as
/// m2v binds it (metal2vulkan-runtime address_map.rs `sampler_maps`): the pairs are the layout's. A sampler two textures
/// need under different modes fails the dispatch's command buffer ("fails: ", TegraGM20BMetalDirect.mm), as m2v's
/// `SharedSamplerSlot` does.
pub(crate) fn mapped_samplers(
    layout: &PipelineLayout,
    ask: &NmbDirectDispatch,
    state: &BindState,
) -> Result<BindState, String> {
    let mut pairs = Vec::new();
    for b in layout.sets.iter().flat_map(|s| &s.bindings) {
        if let BindingSource::CombinedTextureSampler { texture, sampler }
        | BindingSource::SeparateTexture { texture, sampler } = &b.source
            && texture.stage == Stage::Kernel
            && let SamplerRef::Slot(index) = &sampler.sampler
        {
            pairs.push((texture.index, *index));
        }
    }
    let samplers = counted(ask.samplers, ask.sampler_count);
    let modes_of = |slot: u32| {
        samplers
            .iter()
            .find(|s| s.slot == slot)
            .map(|s| [s.descriptor.s_address, s.descriptor.t_address, s.descriptor.r_address])
    };
    let maps = address_maps::sampler_maps(&pairs, |slot| bound_map(ask, slot), modes_of).map_err(|shared| {
        format!(
            "fails: kernel sampler slot {} samples texture slots {} and {}, whose address-mode maps give it two sets \
             of modes, which one sampler cannot hold (m2v's SharedSamplerSlot)",
            shared.sampler, shared.textures[0], shared.textures[1]
        )
    })?;
    let mut mapped = state.clone();
    if let Some(table) = mapped.stages.get_mut(&Stage::Kernel) {
        for (slot, map) in maps {
            if let Some(bound) = table.samplers.get_mut(&slot) {
                bound.desc = address_modes(map).sampler(bound.desc);
            }
        }
    }
    Ok(mapped)
}

fn facts_of(t: &NmbDirectBoundTexture) -> Result<TextureFacts, String> {
    let read = kernel_texture(raw::<PixelFormat>(t.pixel_format, "pixel format")?, t.planes);
    let mut facts = TextureFacts::new(raw::<TextureType>(t.texture_type, "texture type")?, read.format);
    facts.address_modes = address_modes(read.map);
    facts.usage = raw::<TextureUsage>(t.usage, "texture usage")?;
    facts.sample_count = u32::try_from(t.samples.max(1)).map_err(e("sample count"))?;
    facts.array_length = u32::try_from(t.array_length.max(1)).map_err(e("array length"))?;
    facts.mipmap_level_count = u32::try_from(t.levels.max(1)).map_err(e("levels"))?;
    facts.two_planes = t.planes & TWO_PLANES != 0;
    Ok(facts)
}

/// `state` with each texture Metal stores no kernel write into (kernel_textures.rs) bound, at a slot the kernel only
/// writes, as a format whose write-only slot the translator binds its neutral at, so the writes store nothing, as
/// Metal's do; `access` is the kernel's declared access of a texture slot. A slot that reads and writes such a texture
/// has no form: its reads are of the image raw converts, which its writes would change.
pub(crate) fn dropped_stores(
    access: impl Fn(u32) -> Option<TextureAccess>,
    ask: &NmbDirectDispatch,
    mut state: BindState,
) -> Result<BindState, String> {
    for t in counted(ask.textures, ask.texture_count) {
        let pf = raw::<PixelFormat>(t.pixel_format, "pixel format")?;
        let Some(a) = access(t.slot).filter(|a| a.write) else {
            continue;
        };
        if kernel_texture(pf, t.planes).stores {
            continue;
        }
        if a.read || a.sample {
            return Err(format!(
                "a kernel that reads and writes texture slot {} of {pf:?}, whose writes Metal stores nothing of",
                t.slot
            ));
        }
        if let Some(BoundTexture::Texture { facts, .. }) = state
            .stages
            .get_mut(&Stage::Kernel)
            .and_then(|table| table.textures.get_mut(&t.slot))
        {
            facts.pixel_format = WRITES_DROPPED;
        }
    }
    Ok(state)
}

pub(crate) fn bind_state(run: Run, dispatch: &NmbDirectDispatch, stage_in: bool) -> Result<BindState, String> {
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
        let bound = if raw::<TextureType>(t.texture_type, "texture type")? == TextureType::TextureBuffer {
            // the texel buffer view raw binds is the texture's own header (Device::createTexelBuffer), not this range
            BoundTexture::Buffer {
                pixel_format: raw::<PixelFormat>(t.pixel_format, "pixel format")?,
                offset: 0,
                length: u64::from(TAG_BYTES),
                bytes_per_row: u64::from(TAG_BYTES),
                resource_id: None,
                swizzle: TextureSwizzleChannels::default(),
            }
        } else {
            BoundTexture::Texture {
                facts: facts_of(t)?,
                resource_id: None,
            }
        };
        table.textures.insert(t.slot, bound);
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

#[cfg(test)]
mod tests {
    use super::*;

    fn bound(slot: u32, pixel_format: u64, planes: u32) -> NmbDirectBoundTexture {
        NmbDirectBoundTexture {
            slot,
            planes,
            pixel_format,
            texture_type: 2,
            usage: 1,
            levels: 1,
            array_length: 1,
            samples: 1,
        }
    }

    // C32's ci_srgb_to_linear_colormatrix over the planes of a 'p420' surface (588, 589): no format of the translator's
    #[test]
    fn a_packed_planes_slot_is_planned_as_the_image_raw_binds_there() {
        for format in [588, 589] {
            let facts = facts_of(&bound(1, format, 0)).unwrap();
            assert_eq!(facts.pixel_format, PixelFormat::RGB10A2Unorm, "format {format}");
            assert_eq!(facts.address_modes, AddressModeMap::IDENTITY);
        }
    }

    // A 4:2:0 converted texture over a P3_D65 surface: raw reads its codes image and samples it under the codes map (3)
    #[test]
    fn a_stated_map_decides_the_samplers_and_a_codes_read_the_format() {
        let facts = facts_of(&bound(0, 500, (1 << 15) | (3 << 8))).unwrap();
        assert_eq!(facts.pixel_format, PixelFormat::RGB84202P);
        assert_eq!(facts.address_modes, address_modes(3));
        assert!(!facts.two_planes, "bit 15 states a map, not two planes");
        let facts = facts_of(&bound(0, 500, 0)).unwrap();
        assert_eq!(
            (facts.pixel_format, facts.address_modes),
            (PixelFormat::YCBCR84202P, address_modes(1))
        );
        assert!(facts_of(&bound(0, 550, 1)).unwrap().two_planes);
    }

    fn kernel_state(textures: &[NmbDirectBoundTexture]) -> BindState {
        let mut table = StageTable::default();
        for t in textures {
            table.textures.insert(
                t.slot,
                BoundTexture::Texture {
                    facts: facts_of(t).unwrap(),
                    resource_id: None,
                },
            );
        }
        let mut state = BindState::default();
        state.stages.insert(Stage::Kernel, table);
        state
    }

    fn format_at(state: &BindState, slot: u32) -> PixelFormat {
        match &state.stages[&Stage::Kernel].textures[&slot] {
            BoundTexture::Texture { facts, .. } => facts.pixel_format,
            _ => panic!("no texture at {slot}"),
        }
    }

    // An M4 Max stores nothing of a kernel's write into an R10Unorm_PACKED or RG10Unorm_PACKED texture, nor into a
    // YCBCR8_422_1P one: a slot the kernel only writes is bound as a format whose writes the translator drops, one it
    // reads too is refused, and any other texture's writes are planned as they were
    #[test]
    fn a_kernels_writes_into_a_texture_metal_stores_none_of_store_nothing() {
        let textures = [bound(0, 70, 0), bound(3, 588, 0), bound(4, 501, 0)];
        let ask = NmbDirectDispatch {
            threads: [32, 8, 1],
            memory: [0; 31],
            textures: textures.as_ptr(),
            texture_count: 3,
            samplers: std::ptr::null(),
            sampler_count: 0,
            region_mode: 0,
        };
        let write = TextureAccess::new(false, false, true);
        let state = dropped_stores(|_| Some(write), &ask, kernel_state(&textures)).unwrap();
        assert_eq!(format_at(&state, 0), PixelFormat::RGBA8Unorm);
        assert_eq!(format_at(&state, 3), WRITES_DROPPED);
        assert_eq!(format_at(&state, 4), WRITES_DROPPED);
        let read = |slot| Some(TextureAccess::new(false, slot == 3, true));
        let why = dropped_stores(read, &ask, kernel_state(&textures)).unwrap_err();
        assert!(why.contains("texture slot 3 of R10UnormPACKED"), "{why}");
        let sampled = |slot| Some(TextureAccess::new(true, false, slot == 0));
        let state = dropped_stores(sampled, &ask, kernel_state(&textures)).unwrap();
        assert_eq!(
            format_at(&state, 3),
            PixelFormat::RGB10A2Unorm,
            "a slot only read keeps the image raw binds"
        );
    }
}
