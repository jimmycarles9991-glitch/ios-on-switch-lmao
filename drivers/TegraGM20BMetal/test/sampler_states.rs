// SPDX-License-Identifier: MIT
//! samplercheck's sweep of Metal sampler descriptors (pack/probe.mm PROBE_SAMPLERS: min and mag filters, mip filter,
//! address modes, anisotropy 1, 2, 8 and 16, default or [0.5, 3.25] level clamps), each as the translator hands it to a
//! device of the build's compile profile (metal::sampler::vk_sampler), one line per sampler: the sweep's indices, then
//! the Vulkan state kits/SwitchGM20B/SwitchGM20BSampler.h packs, floats as their bits.
//! usage: sampler_states <compile_profile.json>

use metal2vulkan::bind::SamplerDescriptor;
use metal2vulkan::device::{Device, DeviceDescription, DevicePolicy};
use metal2vulkan::metal::sampler::{SamplerUse, vk_sampler};
use metal2vulkan::metal::{SamplerAddressMode, SamplerMinMagFilter, SamplerMipFilter};

#[derive(serde::Deserialize)]
struct Profile {
    enabled: DeviceDescription,
    policy: DevicePolicy,
}

fn metal<T: TryFrom<u32>>(value: u32) -> T {
    T::try_from(value).unwrap_or_else(|_| panic!("no Metal value {value}"))
}

fn main() {
    let path = std::env::args()
        .nth(1)
        .expect("usage: sampler_states <compile_profile.json>");
    let text = std::fs::read_to_string(&path).expect("the compile profile");
    let profile: Profile = serde_json::from_str(&text).expect("a compile profile");
    let device = Device::new(profile.enabled, profile.policy).expect("a compile device");
    println!(
        "# minmag mip addr aniso lod | mag min mipmap u v w anisotropy max_anisotropy compare_op min_lod max_lod border reduction"
    );
    for minmag in 0..4u32 {
        for mip in 0..3u32 {
            for addr in 0..5u32 {
                for aniso in [1u32, 2, 8, 16] {
                    for lod in 0..2u32 {
                        let mut d = SamplerDescriptor::new();
                        d.min_filter = metal::<SamplerMinMagFilter>(minmag & 1);
                        d.mag_filter = metal::<SamplerMinMagFilter>(minmag >> 1);
                        d.mip_filter = metal::<SamplerMipFilter>(mip);
                        d.s_address_mode = metal::<SamplerAddressMode>(addr);
                        d.t_address_mode = metal::<SamplerAddressMode>(if addr == 4 { 4 } else { (addr + 1) % 4 });
                        d.r_address_mode = metal::<SamplerAddressMode>(addr);
                        d.max_anisotropy = aniso;
                        if lod == 1 {
                            d.lod_min_clamp = 0.5;
                            d.lod_max_clamp = 3.25;
                        }
                        let s = vk_sampler(&d, SamplerUse::new(false, false), device.profile()).expect("a sampler");
                        println!(
                            "{minmag} {mip} {addr} {aniso} {lod} {} {} {} {} {} {} {} {:08x} {} {:08x} {:08x} {} {}",
                            s.mag_filter.as_raw(),
                            s.min_filter.as_raw(),
                            s.mipmap_mode.as_raw(),
                            s.address_mode_u.as_raw(),
                            s.address_mode_v.as_raw(),
                            s.address_mode_w.as_raw(),
                            u32::from(s.max_anisotropy.is_some()),
                            s.max_anisotropy.unwrap_or(1.0).to_bits(),
                            s.compare.map_or(7, |c| c.as_raw()),
                            s.min_lod.to_bits(),
                            s.max_lod.to_bits(),
                            s.border_color.as_raw(),
                            s.reduction.as_raw(),
                        );
                    }
                }
            }
        }
    }
}
