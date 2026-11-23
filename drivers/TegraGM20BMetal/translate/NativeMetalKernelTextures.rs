// SPDX-License-Identifier: MIT
//! How the direct compile of a kernel (NativeMetalDirectCompute.rs) describes a texture its dispatch binds to the
//! translator: as the image the driver binds at the slot, under the address map the runtime samples it with. A
//! two-plane YCbCr texture's image is the translator's own form of its format (metal2vulkan format.rs `ycbcr_repr`),
//! except under kIOSurfaceYCbCrMatrix P3_D65 or DCI_P3, where raw reads a converted or sRGB format as the codes format
//! of its layout (ycbcr.cpp planYcbcr): the map the runtime states then clamps, as only a codes read's
//! does (address_maps.rs CLAMPS). The driver also reads formats the translator holds no texture of through an image of its
//! own: R10Unorm_PACKED and RG10Unorm_PACKED as an RGB10A2Unorm image of their codes (packed_plane.h),
//! YCBCR8_422_1P and its sRGB twin as the two-plane 4:2:2 format of the same reads (ycbcr.cpp kOnePlaneFormats: the
//! same output, image and map). Metal stores nothing of a kernel's write into either kind: on an M4 Max under macOS
//! 27.2.1 the surface's bytes stay as they were, and its validation layer refuses YCBCR8_422_1P's write binding.

use crate::address_maps;
use metal2vulkan::metal::PixelFormat;
use metal2vulkan::metal::format::{ChromaSubsampling, YCbCrReads, ycbcr_format};

// NmbDirectBoundTexture.planes (direct_abi.h): bit 0 an RGB10A8_2P_XR10 texture held in Metal's two planes,
// bits 8-9 the address map the runtime samples it with (Texture::addressMap), bit 15 set where the runtime states it
pub const TWO_PLANES: u32 = 1;
const MAP_SHIFT: u32 = 8;
const MAP_STATED: u32 = 1 << 15;

const CODES_FORMATS: [PixelFormat; 9] = [
    PixelFormat::RGB84202P,
    PixelFormat::RGB84222P,
    PixelFormat::RGB84442P,
    PixelFormat::RGB104202P,
    PixelFormat::RGB104222P,
    PixelFormat::RGB104442P,
    PixelFormat::RGB104202PPACKED,
    PixelFormat::RGB104222PPACKED,
    PixelFormat::RGB104442PPACKED,
];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KernelTexture {
    /// The format the kernel is compiled to read the texture in
    pub format: PixelFormat,
    /// Its address map (address_maps.rs), which the kernel's samplers of it are compiled under
    pub map: u8,
    /// Whether Metal stores a kernel's writes into it
    pub stores: bool,
}

/// The address map of a texture of `pf` that raw samples through an image it converts: a two-plane YCbCr format's
pub fn converted_map(pf: PixelFormat) -> u8 {
    ycbcr_format(pf).map_or(0, |y| {
        address_maps::converted_map(
            !matches!(y.subsampling, ChromaSubsampling::Chroma444),
            matches!(y.reads, YCbCrReads::Codes),
        )
    })
}

/// The map a dispatch states for a texture of `planes`, `None` where it states none (the format's own then)
fn stated_map(planes: u32) -> Option<u8> {
    (planes & MAP_STATED != 0).then_some(((planes >> MAP_SHIFT) & 3) as u8)
}

/// The two-plane format of `pf`'s layout read as codes; `None` where `pf` is no converted or sRGB two-plane format
fn codes_format(pf: PixelFormat) -> Option<PixelFormat> {
    let y = ycbcr_format(pf).filter(|y| matches!(y.reads, YCbCrReads::Converted | YCbCrReads::Srgb))?;
    CODES_FORMATS.into_iter().find(|&c| {
        ycbcr_format(c).is_some_and(|x| x.bits == y.bits && x.subsampling == y.subsampling && x.packed == y.packed)
    })
}

/// A texture of `pf` bound with `planes` (NmbDirectBoundTexture's), as the kernel is compiled to reach it
pub fn kernel_texture(pf: PixelFormat, planes: u32) -> KernelTexture {
    let stated = stated_map(planes);
    let codes = stated.is_some_and(|m| m & address_maps::CLAMPS != 0);
    let (format, stores) = match pf {
        PixelFormat::R10UnormPACKED | PixelFormat::RG10UnormPACKED => (PixelFormat::RGB10A2Unorm, false),
        PixelFormat::YCBCR84221P if !codes => (PixelFormat::YCBCR84222P, false),
        PixelFormat::YCBCR84221PSRGB if !codes => (PixelFormat::YCBCR84222PSRGB, false),
        PixelFormat::YCBCR84221P | PixelFormat::YCBCR84221PSRGB => (PixelFormat::RGB84222P, false),
        _ if codes => (codes_format(pf).unwrap_or(pf), true),
        _ => (pf, true),
    };
    KernelTexture {
        format,
        map: stated.unwrap_or_else(|| converted_map(format)),
        stores,
    }
}

/// The address map the compile takes for a texture of MTLPixelFormat `pixel_format` whose dispatch states none
/// (NativeMetalTranslate.h); 0 for a value that is no pixel format
#[unsafe(no_mangle)]
pub extern "C" fn nmb_kernel_texture_map(pixel_format: u64) -> u8 {
    crate::descriptors::raw::<PixelFormat>(pixel_format, "pixel format").map_or(0, |pf| kernel_texture(pf, 0).map)
}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::metal::format::direct_format;
    use metal2vulkan::vk::Format;

    const WRAPPED: u8 = address_maps::WRAPPED;
    const CLAMPS: u8 = address_maps::CLAMPS;

    // ycbcr.cpp kFormats and kOnePlaneFormats: each format raw converts from an IOSurface's planes, its
    // bits, chroma halved across, packed, reads (0 converted, 1 sRGB, 2 codes) and one plane
    const RAW: [(u64, u8, bool, bool, u8, bool); 29] = [
        (500, 8, true, false, 0, false),
        (502, 8, true, false, 0, false),
        (503, 8, false, false, 0, false),
        (505, 10, true, false, 0, false),
        (506, 10, true, false, 0, false),
        (507, 10, false, false, 0, false),
        (508, 10, true, true, 0, false),
        (509, 10, true, true, 0, false),
        (510, 10, false, true, 0, false),
        (520, 8, true, false, 1, false),
        (522, 8, true, false, 1, false),
        (523, 8, false, false, 1, false),
        (525, 10, true, false, 1, false),
        (526, 10, true, false, 1, false),
        (527, 10, false, false, 1, false),
        (528, 10, true, true, 1, false),
        (529, 10, true, true, 1, false),
        (530, 10, false, true, 1, false),
        (540, 8, true, false, 2, false),
        (541, 8, true, false, 2, false),
        (542, 8, false, false, 2, false),
        (543, 10, true, false, 2, false),
        (544, 10, true, false, 2, false),
        (545, 10, false, false, 2, false),
        (546, 10, true, true, 2, false),
        (547, 10, true, true, 2, false),
        (548, 10, false, true, 2, false),
        (501, 8, true, false, 0, true),
        (521, 8, true, false, 1, true),
    ];

    fn pixel_format(raw: u64) -> PixelFormat {
        crate::descriptors::raw::<PixelFormat>(raw, "pixel format").unwrap()
    }

    fn stating(map: u8) -> u32 {
        MAP_STATED | (u32::from(map) << MAP_SHIFT)
    }

    /// ycbcr.cpp imageFormat's image of a texture that reads `reads` in `bits`-bit codes, as its Vulkan format
    fn raw_image(reads: u8, bits: u8) -> Format {
        match (reads, bits) {
            (0, _) => Format::R16G16B16A16_SFLOAT,
            (1, _) => Format::R32G32B32A32_SFLOAT,
            (_, 8) => Format::R8G8B8A8_UNORM,
            _ => Format::A2B10G10R10_UNORM_PACK32,
        }
    }

    // What the kernel reads at the slot is the image raw binds there (compute.cpp: a view of another texel size is
    // no dispatch), and its samplers are made under the map raw samples it with (ycbcr.cpp addressMapOf), for every
    // format raw converts, under every matrix but P3 and under P3, where a converted or sRGB format reads as codes
    #[test]
    fn every_converted_texture_is_read_as_the_image_raw_binds_under_raws_map() {
        for (raw, bits, halved, _, reads, _) in RAW {
            for p3 in [false, true] {
                let read = if p3 { 2 } else { reads };
                let map = (if halved { WRAPPED } else { 0 }) | if read == 2 { CLAMPS } else { 0 };
                let t = kernel_texture(pixel_format(raw), stating(map));
                assert_eq!(
                    direct_format(t.format),
                    Some(raw_image(read, bits)),
                    "format {raw}, P3 {p3}"
                );
                assert_eq!(t.map, map, "format {raw}, P3 {p3}");
                assert_eq!(converted_map(t.format), map, "format {raw}, P3 {p3}: its own map");
            }
        }
    }

    // packed_plane.h planPackedPlane: the image is RGB10A2Unorm of the codes, whatever the matrix, map 0
    #[test]
    fn a_packed_10_bit_plane_is_read_as_its_rgb10a2_image() {
        for raw in [588, 589] {
            let t = kernel_texture(pixel_format(raw), stating(0));
            assert_eq!(
                direct_format(t.format),
                Some(Format::A2B10G10R10_UNORM_PACK32),
                "format {raw}"
            );
            assert_eq!(t.map, 0);
            assert_eq!(
                kernel_texture(pixel_format(raw), 0).format,
                t.format,
                "format {raw}: no map stated"
            );
        }
    }

    #[test]
    fn metal_stores_no_kernel_write_into_a_packed_plane_or_a_one_plane_422_texture() {
        for raw in [588, 589, 501, 521] {
            assert!(!kernel_texture(pixel_format(raw), 0).stores, "format {raw}");
            assert!(
                !kernel_texture(pixel_format(raw), stating(CLAMPS | WRAPPED)).stores,
                "format {raw}"
            );
        }
        for raw in [70, 500, 540, 503] {
            assert!(kernel_texture(pixel_format(raw), stating(converted_map(pixel_format(raw)))).stores);
        }
    }

    // A dispatch that states no map (a request made before the runtime stated them) compiles under its format's own
    #[test]
    fn a_texture_of_no_stated_map_takes_its_formats() {
        let t = kernel_texture(PixelFormat::YCBCR84202P, TWO_PLANES);
        assert_eq!((t.format, t.map), (PixelFormat::YCBCR84202P, WRAPPED));
        let t = kernel_texture(PixelFormat::YCBCR84221P, 0);
        assert_eq!((t.format, t.map), (PixelFormat::YCBCR84222P, WRAPPED));
        assert_eq!(kernel_texture(PixelFormat::RGBA8Unorm, 0).map, 0);
    }

    // The map a corpus key that states none is rekeyed with (pack/kernel_shapes.h stateFormatMaps): what the device
    // samples a texture of the format with under every matrix but P3
    #[test]
    fn a_format_states_the_map_raw_samples_it_with() {
        for (raw, _, halved, _, reads, _) in RAW {
            let map = (if halved { WRAPPED } else { 0 }) | if reads == 2 { CLAMPS } else { 0 };
            assert_eq!(nmb_kernel_texture_map(raw), map, "format {raw}");
        }
        for raw in [588, 589, 70, 115, 555, 0, 99_999] {
            assert_eq!(nmb_kernel_texture_map(raw), 0, "format {raw}");
        }
    }

    // A planar texture over a 4:4:4 surface (surface_textures.cpp createPlanarTexture) states map 2, RGB8_444_2P's
    #[test]
    fn a_stated_map_is_the_kernels_even_where_it_is_not_its_formats() {
        let t = kernel_texture(PixelFormat::RGB84442P, stating(CLAMPS));
        assert_eq!((t.format, t.map), (PixelFormat::RGB84442P, CLAMPS));
        let t = kernel_texture(PixelFormat::YCBCR84202P, stating(0));
        assert_eq!((t.format, t.map), (PixelFormat::YCBCR84202P, 0));
    }
}
