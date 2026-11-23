// SPDX-License-Identifier: MIT
//! The address-mode maps of the textures the driver samples through an image it converts from their planes, for the
//! direct compile of a kernel that binds them (NativeMetalDirectCompute.rs), so the kernel compiles as m2v's variant for
//! them does: its constexpr samplers through the translator's `VariantKey::address_maps`, its runtime samplers mapped at
//! bind (metal2vulkan-runtime address_map.rs). A map is two bits, as raw's `Texture::addressMap`
//! (drivers/TegraGM20BMetal/ycbcr.cpp `addressMapOf`): bit 0 sends the wrapping modes to clamp to edge,
//! bit 1 the clamps to zero and to the border colour. The rule is m2v's `ycbcr_modes` (metal2vulkan-runtime
//! texture/ycbcr_image.rs, a private function of a frozen crate), Metal's reads in EVs XRR2-ycc2-m4, TXN2-ycr1-m4,
//! TXN2-ycb1-m4 and -m3, TXN2-ycq1-m4 and TXN2-ycb2-m4: a 4:2:0 or 4:2:2 texture samples repeat, mirrored repeat and
//! mirror-clamp-to-edge as clamp to edge, a 4:4:4 one as themselves; one read as codes samples clamp to zero and clamp to
//! the border colour as clamp to edge too. Every other texture's map is none (raw's packed 10-bit planes among them).

pub const WRAPPED: u8 = 1;
pub const CLAMPS: u8 = 2;

// MTLSamplerAddressMode
const CLAMP_TO_EDGE: u64 = 0;
const MIRROR_CLAMP_TO_EDGE: u64 = 1;
const REPEAT: u64 = 2;
const MIRROR_REPEAT: u64 = 3;
const CLAMP_TO_ZERO: u64 = 4;
const CLAMP_TO_BORDER_COLOR: u64 = 5;

/// A two-plane YCbCr texture's map: `halved`, its chroma subsampled across (4:2:0, 4:2:2); `codes`, read as codes
/// (`RGB*_2P`)
pub fn converted_map(halved: bool, codes: bool) -> u8 {
    (if halved { WRAPPED } else { 0 }) | if codes { CLAMPS } else { 0 }
}

/// The MTLSamplerAddressMode a sampler stating `mode` samples a texture of map `map` with
pub fn mapped_mode(mode: u64, map: u8) -> u64 {
    let wraps = matches!(mode, MIRROR_CLAMP_TO_EDGE | REPEAT | MIRROR_REPEAT);
    let clamps = matches!(mode, CLAMP_TO_ZERO | CLAMP_TO_BORDER_COLOR);
    if (wraps && map & WRAPPED != 0) || (clamps && map & CLAMPS != 0) {
        CLAMP_TO_EDGE
    } else {
        mode
    }
}

/// A runtime sampler slot whose textures' maps give it two sets of modes, which its one state cannot be (m2v's
/// `MappedSampling::SharedSamplerSlot`)
#[derive(Debug, PartialEq, Eq)]
pub struct SharedSampler {
    pub sampler: u32,
    pub textures: [u32; 2],
}

/// The map each runtime sampler slot takes, of the (texture slot, sampler slot) pairs a kernel samples through: the map
/// of the textures it samples where they give it one set of modes other than its own (`modes_of`, its s, t and r modes;
/// `None`: unbound), in the order the pairs first name each slot. `map_of` is a texture slot's map, 0 where none is bound.
pub fn sampler_maps(
    pairs: &[(u32, u32)],
    map_of: impl Fn(u32) -> u8,
    modes_of: impl Fn(u32) -> Option<[u64; 3]>,
) -> Result<Vec<(u32, u8)>, SharedSampler> {
    let mut out = Vec::new();
    let mut seen: Vec<u32> = Vec::new();
    for &(_, sampler) in pairs {
        if seen.contains(&sampler) {
            continue;
        }
        seen.push(sampler);
        let Some(modes) = modes_of(sampler) else {
            continue;
        };
        let mut first: Option<(u32, u8, [u64; 3])> = None;
        for &(texture, _) in pairs.iter().filter(|p| p.1 == sampler) {
            let map = map_of(texture);
            let mapped = modes.map(|m| mapped_mode(m, map));
            match first {
                None => first = Some((texture, map, mapped)),
                Some((other, _, before)) if before != mapped => {
                    return Err(SharedSampler {
                        sampler,
                        textures: [other, texture],
                    });
                }
                Some(_) => {}
            }
        }
        if let Some((_, map, mapped)) = first
            && mapped != modes
        {
            out.push((sampler, map));
        }
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    // ycbcr.cpp kFormats: each two-plane format's subsampling and reads, and the map raw gives it
    const RAW: [(u64, bool, bool, u8); 27] = [
        (500, true, false, 1),
        (502, true, false, 1),
        (503, false, false, 0),
        (505, true, false, 1),
        (506, true, false, 1),
        (507, false, false, 0),
        (508, true, false, 1),
        (509, true, false, 1),
        (510, false, false, 0),
        (520, true, false, 1),
        (522, true, false, 1),
        (523, false, false, 0),
        (525, true, false, 1),
        (526, true, false, 1),
        (527, false, false, 0),
        (528, true, false, 1),
        (529, true, false, 1),
        (530, false, false, 0),
        (540, true, true, 3),
        (541, true, true, 3),
        (542, false, true, 2),
        (543, true, true, 3),
        (544, true, true, 3),
        (545, false, true, 2),
        (546, true, true, 3),
        (547, true, true, 3),
        (548, false, true, 2),
    ];

    #[test]
    fn every_format_raw_converts_takes_raws_map() {
        for (format, halved, codes, map) in RAW {
            assert_eq!(converted_map(halved, codes), map, "format {format}");
        }
    }

    #[test]
    fn a_map_sends_its_modes_to_the_edge_and_keeps_the_rest() {
        assert_eq!(mapped_mode(REPEAT, WRAPPED), CLAMP_TO_EDGE);
        assert_eq!(mapped_mode(MIRROR_REPEAT, WRAPPED), CLAMP_TO_EDGE);
        assert_eq!(mapped_mode(MIRROR_CLAMP_TO_EDGE, WRAPPED), CLAMP_TO_EDGE);
        assert_eq!(mapped_mode(CLAMP_TO_ZERO, WRAPPED), CLAMP_TO_ZERO);
        assert_eq!(mapped_mode(CLAMP_TO_ZERO, CLAMPS), CLAMP_TO_EDGE);
        assert_eq!(mapped_mode(CLAMP_TO_BORDER_COLOR, CLAMPS), CLAMP_TO_EDGE);
        assert_eq!(mapped_mode(REPEAT, CLAMPS), REPEAT);
        for mode in 0..6 {
            assert_eq!(mapped_mode(mode, 0), mode);
        }
    }

    fn maps(slot: u32) -> u8 {
        match slot {
            0 => WRAPPED | CLAMPS,
            2 => WRAPPED,
            _ => 0,
        }
    }

    #[test]
    fn a_sampler_of_one_converted_texture_takes_its_map() {
        let modes = |_| Some([REPEAT, REPEAT, CLAMP_TO_EDGE]);
        assert_eq!(sampler_maps(&[(0, 3)], maps, modes), Ok(vec![(3, WRAPPED | CLAMPS)]));
    }

    #[test]
    fn a_sampler_its_maps_leave_or_no_sampler_takes_none() {
        let edge = |_| Some([CLAMP_TO_EDGE; 3]);
        assert_eq!(sampler_maps(&[(0, 3), (1, 3)], maps, edge), Ok(vec![]));
        assert_eq!(sampler_maps(&[(0, 3)], maps, |_| None), Ok(vec![]));
        assert_eq!(sampler_maps(&[(1, 3)], maps, |_| Some([REPEAT; 3])), Ok(vec![]));
    }

    #[test]
    fn textures_whose_maps_agree_on_the_sampler_share_it() {
        let modes = |_| Some([REPEAT; 3]);
        assert_eq!(
            sampler_maps(&[(0, 3), (2, 3)], maps, modes),
            Ok(vec![(3, WRAPPED | CLAMPS)])
        );
    }

    #[test]
    fn a_sampler_two_maps_give_two_modes_is_named() {
        let modes = |_| Some([REPEAT; 3]);
        assert_eq!(
            sampler_maps(&[(0, 3), (1, 3)], maps, modes),
            Err(SharedSampler {
                sampler: 3,
                textures: [0, 1]
            })
        );
        let zero = |_| Some([CLAMP_TO_ZERO; 3]);
        assert_eq!(
            sampler_maps(&[(2, 4), (0, 4)], maps, zero),
            Err(SharedSampler {
                sampler: 4,
                textures: [2, 0]
            })
        );
    }

    #[test]
    fn each_sampler_is_judged_alone() {
        let modes = |slot| Some(if slot == 5 { [REPEAT; 3] } else { [CLAMP_TO_ZERO; 3] });
        assert_eq!(
            sampler_maps(&[(0, 5), (1, 6), (0, 6)], maps, modes),
            Err(SharedSampler {
                sampler: 6,
                textures: [1, 0]
            })
        );
        assert_eq!(
            sampler_maps(&[(0, 5), (1, 6)], maps, modes),
            Ok(vec![(5, WRAPPED | CLAMPS)])
        );
    }
}
