// SPDX-License-Identifier: MIT

use crate::descriptors::raw;
use crate::direct_abi::{
    NmbDirectBinds, NmbDirectCubicSampler, NmbDirectSource, NmbDirectTextureFormat, NmbDirectWrite, stage_number,
};
use metal2vulkan::bind::{
    BindState, BoundBuffer, BoundSampler, BoundTexture, FragmentTests, SamplerDescriptor, StageTable, TextureFacts,
    VertexAmplification, ViewMapping,
};
use metal2vulkan::metal::{
    PixelFormat, SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter, TextureSwizzleChannels,
    TextureType, TextureUsage,
};
use metal2vulkan::pipeline::{RenderPipeline, SampledType, SlotClass, SlotInterface, SlotRef, Stage, TextureSlot};

const PROBE_SLOTS: u32 = 31;
const PROBE_TAGGED: u32 = 16;
pub(crate) const RUN_BYTES: [u32; 4] = [256, 16, 4096, 256];
const BOUND_RUN: usize = 3;

/// What the probe binds besides its tagged textures and samplers: cubic sampler states, and other textures' formats, by
/// stage and slot
#[derive(Default)]
pub struct Binds {
    cubic: Vec<(Stage, u32, SamplerDescriptor)>,
    textures: Vec<(Stage, u32, PixelFormat)>,
    pub fragment_tests: u32,
    bind_stride: Option<u32>,
}

impl Binds {
    /// # Safety
    /// `binds` is null or valid, its arrays of their counts
    pub unsafe fn of(binds: *const NmbDirectBinds) -> Result<Binds, String> {
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
            out.cubic.push((bound_stage(c.vertex), c.slot, d));
        }
        let textures: &[NmbDirectTextureFormat] = if b.textures.is_null() {
            &[]
        } else {
            // SAFETY: `texture_count` records at `textures`.
            unsafe { std::slice::from_raw_parts(b.textures, b.texture_count as usize) }
        };
        for t in textures {
            out.textures.push((
                bound_stage(t.vertex),
                t.slot,
                raw::<PixelFormat>(t.format, "texture format")?,
            ));
        }
        Ok(out)
    }

    /// The cubic sampler state the probe binds at `stage`'s sampler slot `slot`
    fn cubic_at(&self, stage: Stage, slot: u32) -> Option<SamplerDescriptor> {
        self.cubic
            .iter()
            .find(|(at, s, _)| *at == stage && *s == slot)
            .map(|(_, _, d)| *d)
    }
}

fn bound_stage(vertex: bool) -> Stage {
    if vertex { Stage::Vertex } else { Stage::Fragment }
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

/// The texture slots the probe binds by their declarations: each the state's functions declare at the vertex stage or at
/// a fragment slot past the tagged ones, and each tagged fragment one declared other than a single-sample `texture2d` of
/// floats. A texture of the declared type (Core Animation's colour cubes a 3D one, a `texture2d_ms` or a `depth2d_ms` a
/// multisampled one), sampled type and comparison is bound there, so that each gets its own write at its own Metal index
/// where an unbound or mistyped slot would get the declaration's neutral
pub(crate) fn declared_slots(pipeline: &RenderPipeline) -> Vec<(Stage, u32, TextureSlot)> {
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
pub(crate) fn vertex_samplers(pipeline: &RenderPipeline) -> Vec<u32> {
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

/// What the probe binds at a slot of `texture_type` whose facts are `facts` (`declared_facts`): a texture buffer at a
/// texture-buffer slot, whose view raw binds is the texture's own header (Device::createTexelBuffer), not this range
fn declared_texture(texture_type: TextureType, facts: TextureFacts) -> BoundTexture {
    match texture_type {
        TextureType::TextureBuffer => BoundTexture::Buffer {
            pixel_format: facts.pixel_format,
            offset: 0,
            length: 4096,
            bytes_per_row: 4096,
            resource_id: None,
            swizzle: TextureSwizzleChannels::default(),
        },
        _ => BoundTexture::Texture {
            facts,
            resource_id: None,
        },
    }
}

/// The probe's address of a buffer at slot `slot` of `stage` (1 vertex, 2 fragment)
fn probe_address(stage: u32, slot: u32) -> u64 {
    0x1_0000_0000u64 + (u64::from(stage) << 24) + (u64::from(slot) << 16)
}

/// The bind state of probe.mm's draw `run`: every buffer slot of both stages bound (inline bytes, or the bound run's
/// buffers at each slot's own offset; a buffer of the run's bytes at a slot of `addressed`, which m2v reaches by its
/// device address or whose Metal address its programs read; vertex buffers at `binds`' stride), 16 textures of `format`
/// and `samples` and 16 samplers at the fragment stage, a texture of its declaration at each of `declared` and a sampler
/// at each of `vertex_samplers`, `binds`' in their places
#[allow(clippy::too_many_arguments)]
pub(crate) fn probe_state(
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
    // One draw per amplification, which the driver draws in order, under mappings that offset the viewport (with `layers`
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
                .find(|(at, slot, _)| *at == stage && *slot == t)
                .map(|(_, _, f)| *f)
        };
        for (_, t, d) in declared
            .iter()
            .filter(|(at, t, _)| *at == stage && (stage == Stage::Vertex || *t >= PROBE_TAGGED))
        {
            table.textures.insert(
                *t,
                declared_texture(d.texture_type, declared_facts(d, own_format(*t), format, samples)),
            );
        }
        if stage == Stage::Vertex {
            for &s in vertex_samplers {
                let sampler = binds.cubic_at(Stage::Vertex, s).unwrap_or_else(|| probe_sampler(s));
                table.samplers.insert(s, BoundSampler::new(sampler));
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
                table.textures.insert(t, declared_texture(facts.texture_type, facts));
                let cubic = binds.cubic_at(Stage::Fragment, t);
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
pub(crate) fn probed_format(state: &BindState, w: &NmbDirectWrite) -> u32 {
    if w.stage != 2 || !matches!(w.source, NmbDirectSource::Texture | NmbDirectSource::Combined) {
        return 0;
    }
    match state
        .stages
        .get(&Stage::Fragment)
        .and_then(|table| table.textures.get(&w.slot))
    {
        Some(BoundTexture::Texture { facts, .. }) => facts.pixel_format.as_raw(),
        Some(BoundTexture::Buffer { pixel_format, .. }) => pixel_format.as_raw(),
        _ => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::{
        BindState, BoundTexture, NmbDirectSource, NmbDirectWrite, PixelFormat, Stage, StageTable, TextureFacts,
        TextureType, declared_texture, probed_format,
    };

    // A fragment function's texture-buffer argument is probed as a texture buffer of its format, whose view the binder
    // plans (a texture of the TextureBuffer type has none: "no view type"), and a storage write into it keeps the format
    // its stores are compiled for (PackPipeline::writtenFormat); a 2D argument stays a texture
    #[test]
    fn a_texture_buffer_argument_is_probed_as_a_texture_buffer_of_its_format() {
        let xr = PixelFormat::RGB10A82PXR10;
        let mut table = StageTable::default();
        for (slot, ty) in [(3, TextureType::TextureBuffer), (4, TextureType::Type2D)] {
            table
                .textures
                .insert(slot, declared_texture(ty, TextureFacts::new(ty, xr)));
        }
        assert!(matches!(table.textures[&3], BoundTexture::Buffer { pixel_format, .. } if pixel_format == xr));
        assert!(matches!(table.textures[&4], BoundTexture::Texture { .. }));
        let mut state = BindState::default();
        state.stages.insert(Stage::Fragment, table);
        let write = |slot| NmbDirectWrite {
            set: 0,
            binding: 0,
            element: 0,
            ty: 5,
            source: NmbDirectSource::Texture,
            stage: 2,
            slot,
            sampler: 0,
            range: 0,
            view_type: 0,
            format: 0,
            compare: 0,
        };
        assert_eq!(probed_format(&state, &write(3)), 550);
        assert_eq!(probed_format(&state, &write(4)), 550);
    }
}
