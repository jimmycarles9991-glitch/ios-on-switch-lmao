// SPDX-License-Identifier: MIT
//! Prints drivers/TegraGM20BMetal/raw/runtime/raw_heap_classes.h: what m2v's heap rules (bind/heap.rs `fit`, through
//! `heap_code_holds`, and `handle_entry_flags`) read of each Metal pixel format, one row each, in the form
//! raw_heap_fit.h reads it. A row is printed only once it gives the crate's answer under every swizzle.
//!
//!   bazel run //tools/raw_heap_classes > drivers/TegraGM20BMetal/raw/runtime/raw_heap_classes.h
//!   clang-format -i drivers/TegraGM20BMetal/raw/runtime/raw_heap_classes.h
//!
//! `bazel test //tools/raw_heap_classes:raw_heap_classes_test` fails where the checked-in header is not this
//! translator's.

use metal2vulkan::bind::{TextureFacts, handle_entry_flags, heap_code_class, heap_code_holds};
use metal2vulkan::metal::format::{ExtendedRange, is_depth_stencil, sampled_type};
use metal2vulkan::metal::{PixelFormat, TextureSwizzle, TextureSwizzleChannels, TextureType, TextureUsage};
use metal2vulkan::pipeline::{HandleClass, ImageAccess, ImageDim, SampledType, StorageFormatClass};

use TextureSwizzle as S;

const OWN: [S; 4] = [S::Red, S::Green, S::Blue, S::Alpha];
const LACKING: [S; 4] = [S::Zero, S::Zero, S::Zero, S::One];
const NOT_SAMPLED: u32 = 4;

// heap_code.rs's word of a 2D storage image of half-float bits: an image (1), D2 (1 << 2), storage (bit 10), HalfBits (3)
const HALF_BITS_2D: u32 = 1 | 1 << 2 | 1 << 10 | 3 << 11;

fn swizzle(c: [S; 4]) -> TextureSwizzleChannels {
    TextureSwizzleChannels::new(c[0], c[1], c[2], c[3])
}

fn keyed(key: u32) -> [S; 4] {
    std::array::from_fn(|c| if key >> c & 1 != 0 { LACKING[c] } else { OWN[c] })
}

fn key_of(s: [S; 4]) -> Option<u32> {
    (0..4).try_fold(0, |key, c| match s[c] {
        x if x == OWN[c] => Some(key),
        x if x == LACKING[c] => Some(key | 1 << c),
        _ => None,
    })
}

fn every_swizzle() -> Vec<[S; 4]> {
    let all = S::ALL;
    let mut out = Vec::new();
    for &r in all {
        for &g in all {
            for &b in all {
                for &a in all {
                    out.push([r, g, b, a]);
                }
            }
        }
    }
    out
}

fn facts(pf: PixelFormat, s: [S; 4], usage: TextureUsage) -> TextureFacts {
    let mut f = TextureFacts::new(TextureType::Type2D, pf);
    f.usage = usage;
    f.swizzle = swizzle(s);
    f
}

fn crate_flags(pf: PixelFormat, s: [S; 4]) -> u32 {
    handle_entry_flags(&facts(pf, s, TextureUsage::SHADER_READ))
}

fn crate_half_bits(pf: PixelFormat, s: [S; 4]) -> bool {
    heap_code_holds(HALF_BITS_2D, &facts(pf, s, TextureUsage::SHADER_WRITE))
}

fn sampled_field(t: Option<SampledType>) -> u32 {
    match t {
        Some(SampledType::Float) => 0,
        Some(SampledType::Sint) => 1,
        Some(SampledType::Uint) => 2,
        Some(SampledType::Uint64) => 3,
        None => NOT_SAMPLED,
        Some(t) => panic!("a sampled type heap_code.rs has no field for: {t:?}"),
    }
}

struct Row {
    mtl: u32,
    sampled: u32,
    depth_stencil: bool,
    heap_read: bool,
    half_bits: u16,
    alike: u16,
    own: u32,
    alt: u32,
    other: u32,
}

impl Row {
    fn of(pf: PixelFormat) -> Row {
        let other = crate_flags(pf, [S::Green, S::Red, S::Blue, S::Alpha]);
        let (mut alike, mut alt) = (0u16, 0u32);
        for key in 1..16 {
            let flags = crate_flags(pf, keyed(key));
            if flags != other {
                assert!(
                    alike == 0 || flags == alt,
                    "{pf:?}: a third flags word, {flags:#x}, at key {key}"
                );
                alike |= 1 << key;
                alt = flags;
            }
        }
        let half_bits = (0..16)
            .filter(|&k| crate_half_bits(pf, keyed(k)))
            .fold(0u16, |m, k| m | 1 << k);
        let row = Row {
            mtl: pf.as_raw(),
            sampled: sampled_field(sampled_type(pf)),
            depth_stencil: is_depth_stencil(pf),
            heap_read: ExtendedRange::of(pf).and_then(ExtendedRange::heap_read).is_some(),
            half_bits,
            alike,
            own: crate_flags(pf, OWN),
            alt,
            other,
        };
        for s in every_swizzle() {
            assert_eq!(row.flags(s), crate_flags(pf, s), "{pf:?} {s:?}");
            assert_eq!(row.half_bits_held(s), crate_half_bits(pf, s), "{pf:?} {s:?}");
        }
        row
    }

    fn flags(&self, s: [S; 4]) -> u32 {
        match key_of(s) {
            Some(0) => self.own,
            Some(k) if self.alike >> k & 1 != 0 => self.alt,
            _ => self.other,
        }
    }

    fn half_bits_held(&self, s: [S; 4]) -> bool {
        key_of(s).is_some_and(|k| self.half_bits >> k & 1 != 0)
    }
}

fn check_words() {
    match heap_code_class(HALF_BITS_2D) {
        Some(HandleClass::Image(c)) => assert!(
            c.dim == ImageDim::D2
                && !c.arrayed
                && !c.multisampled
                && !c.depth_compare
                && c.access == ImageAccess::Storage
                && c.format == StorageFormatClass::HalfBits
        ),
        other => panic!("HALF_BITS_2D decodes to {other:?}"),
    }
    for field in 0..NOT_SAMPLED {
        let Some(HandleClass::Image(c)) = heap_code_class(1 | field << 7) else {
            panic!("no image class of sampled field {field}");
        };
        assert_eq!(sampled_field(Some(c.sampled_type)), field);
    }
}

fn contiguous(raw: impl Iterator<Item = u32>) -> u32 {
    let mut n = 0;
    for (i, v) in raw.enumerate() {
        assert_eq!(v as usize, i, "not 0..n");
        n += 1;
    }
    n
}

fn list(values: impl Iterator<Item = u32>) -> String {
    values.map(|v| v.to_string()).collect::<Vec<_>>().join(", ")
}

fn render() -> String {
    use std::fmt::Write;
    let mut out = String::new();
    check_words();
    let mut rows: Vec<Row> = PixelFormat::ALL.iter().map(|&pf| Row::of(pf)).collect();
    rows.sort_by_key(|r| r.mtl);
    let types = contiguous(TextureType::ALL.iter().map(|t| t.as_raw()));
    let swizzles = contiguous(S::ALL.iter().map(|s| s.as_raw()));
    let taken = TextureUsage::ALL_KNOWN.as_raw() | TextureUsage::DROPPED;

    write!(
        out,
        "\
// SPDX-License-Identifier: MIT
// Generated by tools/raw_heap_classes (its src/main.rs says how) from m2v's translator: do not edit. What m2v's heap
// rules (metal2vulkan bind/heap.rs fit and entry_flags) read of each Metal pixel format, a row per MTLPixelFormat
// enumerator m2v names, which raw_heap_fit.h reads. A swizzle's key: bit c set where channel c reads kLacking[c], the
// value a channel the format lacks reads anyway, and clear where it reads kOwn[c]; a swizzle with a channel that reads
// neither has no key.
//  - sampled: m2v's SampledType as a class word's field (Float 0, Sint 1, Uint 2, Uint64 3), kNotSampled for none.
//  - heapRead: a form of m2v's ExtendedRange::HEAP_READ, whose unswizzled single-sample texture the unsigned integer class
//    holds by its raw texels.
//  - halfBits: the keys under which the half-bits class (StorageFormatClass::HalfBits) holds the texture.
//  - own: handle_entry_flags under key 0 (the default swizzle); alt: under each key of `alike`; other: under every other
//    swizzle.
#pragma once
#include <cstdint>

namespace raw::heap_classes {{

constexpr uint32_t kTextureTypes = {types}, kSwizzles = {swizzles};
constexpr uint8_t kOwn[4] = {{{own}}}, kLacking[4] = {{{lacking}}};
constexpr uint32_t kUsageTaken = {taken:#x}, kShaderRead = {read:#x}, kShaderWrite = {write:#x}, kBlockWritesOnly = {block:#x};
constexpr uint8_t kNotSampled = {NOT_SAMPLED};

struct Format {{
    uint16_t mtl;
    uint8_t sampled;
    bool depthStencil, heapRead;
    uint16_t halfBits, alike;
    uint32_t own, alt, other;
}};

inline constexpr Format kFormats[] = {{
",
        own = list(OWN.iter().map(|s| s.as_raw())),
        lacking = list(LACKING.iter().map(|s| s.as_raw())),
        read = TextureUsage::SHADER_READ.as_raw(),
        write = TextureUsage::SHADER_WRITE.as_raw(),
        block = TextureUsage::BLOCK_WRITES_ONLY.as_raw(),
    )
    .expect("a String takes every write");
    for r in &rows {
        writeln!(
            out,
            "    {{{}, {}, {}, {}, {:#06x}, {:#06x}, {:#010x}, {:#010x}, {:#010x}}},",
            r.mtl,
            r.sampled,
            u8::from(r.depth_stencil),
            u8::from(r.heap_read),
            r.half_bits,
            r.alike,
            r.own,
            r.alt,
            r.other
        )
        .expect("a String takes every write");
    }
    out.push_str("};\n\n}  // namespace raw::heap_classes\n");
    out
}

fn main() {
    print!("{}", render());
}

// The checked-in header is this crate's output under clang-format, which moves only whitespace: a header another
// translator's rules made (a stale table) differs in a row or a constant
#[cfg(test)]
mod tests {
    fn tokens(text: &str) -> String {
        text.chars().filter(|c| !c.is_whitespace()).collect()
    }

    #[test]
    fn the_checked_in_header_is_this_translators() {
        let path = std::env::var("RAW_HEAP_CLASSES_H").expect("RAW_HEAP_CLASSES_H names the checked-in header");
        let checked_in = std::fs::read_to_string(&path).expect("the checked-in header");
        assert!(
            tokens(&checked_in) == tokens(&super::render()),
            "{path} is not this translator's: regenerate it with \
             `bazel run //tools/raw_heap_classes > {path}` and clang-format it"
        );
    }
}
