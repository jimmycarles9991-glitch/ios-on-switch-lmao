// SPDX-License-Identifier: MIT
//! The raw GM20B path's direct compile with no m2v runtime: a render state's or a kernel's translation by metal2vulkan's
//! translator alone, on the compile profile's devices, handed to the stage compile as SPIR-V and layouts given as data
//! (NativeMetalDirect.h). The pack generator and the shader compiler daemon compile through it; the m2v bridge
//! (NativeMetalBridge.rs) adds its runtime device's forms on top.

#[path = "NativeMetalAddressMaps.rs"]
mod address_maps;
#[path = "NativeMetalArgumentLayouts.rs"]
pub mod argument_layouts;
#[path = "NativeMetalDescriptors.rs"]
pub mod descriptors;
#[path = "NativeMetalDirect.rs"]
pub mod direct;
#[path = "NativeMetalDirectCompute.rs"]
pub mod direct_compute;
#[path = "NativeMetalDirectTess.rs"]
mod direct_tess;
#[path = "NativeMetalOffline.rs"]
pub mod offline;
#[path = "NativeMetalReflection.rs"]
pub mod reflection;
#[path = "NativeMetalTessModes.rs"]
mod tess_modes;

/// The budget of every compile the translator and the bridge start: no deadline, so the translator's deterministic
/// work-unit cap and memory ledger bound it alone. A wall-clock deadline fails a compile that runs long under load
/// ("Budget(Time)") and passes the same compile later.
pub fn compile_budget() -> metal2vulkan::Budget {
    metal2vulkan::Budget::default()
}

/// The AIR slice of a universal metallib (a fat container, big endian, of an AIR64 metallib and an Apple-GPU binary, as the target's
/// frameworks ship them), as Metal itself picks it; a thin metallib is returned as it is.
pub fn air_slice(bytes: &[u8]) -> Option<&[u8]> {
    const AIR64: u32 = 0x0100_0017;
    let word = |at: usize| {
        bytes
            .get(at..at + 4)
            .map(|w| u32::from_be_bytes([w[0], w[1], w[2], w[3]]))
    };
    if word(0)? != 0xcafe_babe {
        return Some(bytes);
    }
    let count = word(4)? as usize;
    for i in 0..count.min(16) {
        let at = 8 + 20 * i;
        if word(at)? == AIR64 {
            let (offset, size) = (word(at + 8)? as usize, word(at + 12)? as usize);
            return bytes.get(offset..offset.checked_add(size)?);
        }
    }
    None
}
