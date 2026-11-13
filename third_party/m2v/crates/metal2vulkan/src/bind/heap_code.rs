//! A set-1 heap array's class as one `u32` word, for a consumer that keeps one heap set of every
//! live texture and sampler (`HeapPlacement::ResourceId`'s device-wide form, [`heap_element`]) and
//! states a compiled state's arrays apart from the translator: [`heap_class_code`] names the class
//! of a layout's `BindingSource::Heap` binding, [`heap_code_holds`] answers whether an array of that
//! class holds a texture at its element (the class's neutral otherwise), as a plan's member fits.
//!
//! The word: bits 0-1 the kind (1 an image class, 2 a sampler class). An image class: bits 2-4
//! its `ImageDim`, bit 5 arrayed, bit 6 multisampled, bits 7-8 its `SampledType`, bit 9 depth
//! comparison, bit 10 storage access, bits 11-12 its `StorageFormatClass` (not storage,
//! formatless, a format, half bits) and bits 13-31 a storage format's `VkFormat` value. A sampler
//! class: bit 2 comparison, bit 3 an integer texture's, bit 4 a cube's explicit gradients. 0 names
//! no class. Every field is the type's own, so a word stays the class it was across builds.
//!
//! [`heap_element`]: super::heap::heap_element

use super::heap::{fit, Fit};
use super::state::TextureFacts;
use crate::pipeline::sym::{
    HandleClass, ImageAccess, ImageDim, ImageHandleClass, SampledType, StorageFormatClass,
};
use crate::vk;

const IMAGE: u32 = 1;
const SAMPLER: u32 = 2;
const FORMAT_SHIFT: u32 = 13;

/// `class` as one word (the module docs); `None` for a class the word cannot state: a
/// function-table array, and a storage format past the word's 19 bits.
pub fn heap_class_code(class: &HandleClass) -> Option<u32> {
    match *class {
        HandleClass::Image(image) => image_code(&image),
        HandleClass::Sampler {
            compare,
            integer,
            cube_gradient,
        } => Some(
            SAMPLER
                | u32::from(compare) << 2
                | u32::from(integer) << 3
                | u32::from(cube_gradient) << 4,
        ),
        HandleClass::FunctionTable => None,
    }
}

fn image_code(c: &ImageHandleClass) -> Option<u32> {
    let dim = match c.dim {
        ImageDim::D1 => 0,
        ImageDim::D2 => 1,
        ImageDim::D3 => 2,
        ImageDim::Cube => 3,
        ImageDim::Buffer => 4,
    };
    let sampled = match c.sampled_type {
        SampledType::Float => 0,
        SampledType::Sint => 1,
        SampledType::Uint => 2,
        SampledType::Uint64 => 3,
    };
    let (format, raw) = match c.format {
        StorageFormatClass::NotStorage => (0, 0),
        StorageFormatClass::Unknown => (1, 0),
        StorageFormatClass::Format(f) => (2, u32::try_from(f.as_raw()).ok()?),
        StorageFormatClass::HalfBits => (3, 0),
    };
    if raw >= 1 << (32 - FORMAT_SHIFT) {
        return None;
    }
    Some(
        IMAGE
            | dim << 2
            | u32::from(c.arrayed) << 5
            | u32::from(c.multisampled) << 6
            | sampled << 7
            | u32::from(c.depth_compare) << 9
            | u32::from(c.access == ImageAccess::Storage) << 10
            | format << 11
            | raw << FORMAT_SHIFT,
    )
}

/// The class `code` names; `None` for a word [`heap_class_code`] gives no class.
pub fn heap_code_class(code: u32) -> Option<HandleClass> {
    match code & 3 {
        IMAGE => image_class(code).map(HandleClass::Image),
        SAMPLER if code >> 5 == 0 => Some(HandleClass::Sampler {
            compare: code >> 2 & 1 != 0,
            integer: code >> 3 & 1 != 0,
            cube_gradient: code >> 4 & 1 != 0,
        }),
        _ => None,
    }
}

fn image_class(code: u32) -> Option<ImageHandleClass> {
    let dim = match code >> 2 & 7 {
        0 => ImageDim::D1,
        1 => ImageDim::D2,
        2 => ImageDim::D3,
        3 => ImageDim::Cube,
        4 => ImageDim::Buffer,
        _ => return None,
    };
    let sampled_type = match code >> 7 & 3 {
        0 => SampledType::Float,
        1 => SampledType::Sint,
        2 => SampledType::Uint,
        _ => SampledType::Uint64,
    };
    let raw = code >> FORMAT_SHIFT;
    let format = match code >> 11 & 3 {
        0 if raw == 0 => StorageFormatClass::NotStorage,
        1 if raw == 0 => StorageFormatClass::Unknown,
        2 => StorageFormatClass::Format(vk::Format::from_raw(i32::try_from(raw).ok()?)),
        3 if raw == 0 => StorageFormatClass::HalfBits,
        _ => return None,
    };
    Some(ImageHandleClass {
        dim,
        arrayed: code >> 5 & 1 != 0,
        multisampled: code >> 6 & 1 != 0,
        sampled_type,
        depth_compare: code >> 9 & 1 != 0,
        access: if code >> 10 & 1 != 0 {
            ImageAccess::Storage
        } else {
            ImageAccess::Sampled
        },
        format,
    })
}

/// Whether the image array of class `code` holds a texture of `facts` at its element, as a plan's
/// heap member fits its class (`Fit::Holds`); false for a sampler class and a word that names no
/// class. A texture of a form a heap reads by its raw texels is held by the unsigned integer class
/// alone, as a plan holds it.
pub fn heap_code_holds(code: u32, facts: &TextureFacts) -> bool {
    match heap_code_class(code) {
        Some(HandleClass::Image(image)) => fit(&facts.shape(), &image) == Fit::Holds,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::metal::{PixelFormat, TextureType, TextureUsage};

    fn image_classes() -> Vec<HandleClass> {
        let mut out = Vec::new();
        for dim in [
            ImageDim::D1,
            ImageDim::D2,
            ImageDim::D3,
            ImageDim::Cube,
            ImageDim::Buffer,
        ] {
            for sampled_type in [
                SampledType::Float,
                SampledType::Sint,
                SampledType::Uint,
                SampledType::Uint64,
            ] {
                for access in [ImageAccess::Sampled, ImageAccess::Storage] {
                    for format in [
                        StorageFormatClass::NotStorage,
                        StorageFormatClass::Unknown,
                        StorageFormatClass::HalfBits,
                        StorageFormatClass::Format(vk::Format::R32G32B32A32_SFLOAT),
                    ] {
                        for bits in 0..8u32 {
                            out.push(HandleClass::Image(ImageHandleClass {
                                dim,
                                arrayed: bits & 1 != 0,
                                multisampled: bits & 2 != 0,
                                sampled_type,
                                depth_compare: bits & 4 != 0,
                                access,
                                format,
                            }));
                        }
                    }
                }
            }
        }
        out
    }

    #[test]
    fn every_class_comes_back_from_its_word() {
        let mut classes = image_classes();
        for bits in 0..8u32 {
            classes.push(HandleClass::Sampler {
                compare: bits & 1 != 0,
                integer: bits & 2 != 0,
                cube_gradient: bits & 4 != 0,
            });
        }
        let mut words = std::collections::BTreeSet::new();
        for class in &classes {
            let code = heap_class_code(class).expect("a word");
            assert_eq!(heap_code_class(code), Some(*class), "{class:?}");
            assert!(words.insert(code), "{class:?} shares its word");
        }
        assert_eq!(heap_class_code(&HandleClass::FunctionTable), None);
        assert_eq!(heap_code_class(0), None);
    }

    #[test]
    fn a_word_holds_the_textures_its_class_fits() {
        let sampled_2d = HandleClass::Image(ImageHandleClass {
            dim: ImageDim::D2,
            arrayed: false,
            multisampled: false,
            sampled_type: SampledType::Float,
            depth_compare: false,
            access: ImageAccess::Sampled,
            format: StorageFormatClass::NotStorage,
        });
        let code = heap_class_code(&sampled_2d).unwrap();
        let mut facts = TextureFacts::new(TextureType::Type2D, PixelFormat::RGBA8Unorm);
        facts.usage = TextureUsage::SHADER_READ;
        assert!(heap_code_holds(code, &facts));
        facts.texture_type = TextureType::Type2DArray;
        assert!(!heap_code_holds(code, &facts));
        let sampler = HandleClass::Sampler {
            compare: false,
            integer: false,
            cube_gradient: false,
        };
        assert!(!heap_code_holds(heap_class_code(&sampler).unwrap(), &facts));
        assert!(!heap_code_holds(0, &facts));
    }
}
