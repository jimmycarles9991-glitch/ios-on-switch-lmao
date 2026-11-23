// SPDX-License-Identifier: MIT

use crate::descriptors::e;
use crate::direct_abi::{NMB_DIRECT_IMMUTABLE, NmbDirectSamplerState};
use crate::direct_compute_abi::NmbDirectKernelWrite;
use crate::direct_layout::sampler_state;
use crate::direct_writes::write_of;
use ash::vk as avk;
use metal2vulkan::bind::{BindState, DispatchCommands, ImageSource, NeutralResource, SamplerSource, WriteSource};
use metal2vulkan::metal::TextureSwizzleChannels;
use metal2vulkan::pipeline::{BindingSource, PipelineLayout, Stage};

/// The samplers a dispatch's sampler writes take: their states, and the Vulkan samplers made of them where there is a
/// device, destroyed with it
pub(crate) struct Samplers<'d> {
    pub(crate) device: Option<&'d ash::Device>,
    pub(crate) made: Vec<avk::Sampler>,
}

impl Samplers<'_> {
    /// The sampler a write takes (its handle, 0 with no device, and its state); none where the dispatch binds none
    fn make(
        &mut self,
        compile: &metal2vulkan::device::Device,
        state: &BindState,
        source: &SamplerSource,
        image_format: Option<metal2vulkan::vk::Format>,
    ) -> Result<Option<(u64, NmbDirectSamplerState)>, String> {
        if let SamplerSource::Neutral(NeutralResource::Sampler { compare }) = source {
            let info = avk::SamplerCreateInfo::default()
                .mag_filter(avk::Filter::NEAREST)
                .min_filter(avk::Filter::NEAREST)
                .mipmap_mode(avk::SamplerMipmapMode::NEAREST)
                .address_mode_u(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_v(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .address_mode_w(avk::SamplerAddressMode::CLAMP_TO_EDGE)
                .max_lod(avk::LOD_CLAMP_NONE)
                .compare_enable(*compare)
                .compare_op(avk::CompareOp::NEVER);
            let state = sampler_state(&info, avk::SamplerReductionMode::WEIGHTED_AVERAGE, None);
            return Ok(Some((self.create(&info)?, state)));
        }
        let SamplerSource::Slot {
            slot,
            compare,
            integer,
            opaque_black,
        } = source
        else {
            return Ok(None);
        };
        let Some(bound) = state
            .stages
            .get(&Stage::Kernel)
            .and_then(|table| table.samplers.get(&slot.index))
        else {
            return Ok(None);
        };
        let desc =
            metal2vulkan::bind::sampler_desc(compile, &bound.desc, *compare, *integer, *opaque_black, image_format)
                .map_err(e("sampler"))?;
        let mut reduction = avk::SamplerReductionModeCreateInfo::default();
        let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
        let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
        let made = sampler_state(
            &info,
            desc.reduction.into(),
            desc.border_components.map(avk::ComponentMapping::from),
        );
        Ok(Some((self.create(&info)?, made)))
    }

    fn create(&mut self, info: &avk::SamplerCreateInfo<'_>) -> Result<u64, String> {
        let Some(device) = self.device else {
            return Ok(0);
        };
        // SAFETY: `info` and its chain are live for the call.
        let sampler = unsafe { device.create_sampler(info, None) }.map_err(e("sampler"))?;
        self.made.push(sampler);
        Ok(avk::Handle::as_raw(sampler))
    }
}

impl Drop for Samplers<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        for &sampler in &self.made {
            // SAFETY: made here; NVK only read its header.
            unsafe { device.destroy_sampler(sampler, None) };
        }
    }
}

impl NmbDirectKernelWrite {
    fn take_sampler(&mut self, made: Option<(u64, NmbDirectSamplerState)>) {
        if let Some((sampler, state)) = made {
            (self.sampler, self.sampler_state, self.has_sampler) = (sampler, state, true);
        }
    }
}

fn view_parts(view: &metal2vulkan::bind::ImageViewDesc) -> (u32, [u32; 4]) {
    let components = if view.shader_swizzle {
        view.components
    } else {
        TextureSwizzleChannels::DEFAULT.compose(view.components)
    };
    let c = avk::ComponentMapping::from(components);
    (
        avk::Format::from(view.format).as_raw() as u32,
        [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
    )
}

/// Whether binding `(set, binding)` of `layout` is a two-plane texture's alpha plane
fn alpha_plane(layout: &PipelineLayout, set: u32, binding: u32) -> bool {
    layout
        .sets
        .iter()
        .filter(|s| s.set == set)
        .flat_map(|s| &s.bindings)
        .any(|b| b.binding == binding && matches!(b.source, BindingSource::TexturePlane(_)))
}

pub(crate) fn kernel_write(
    w: &metal2vulkan::bind::DescriptorWrite,
    (commands, layout): (&DispatchCommands, &PipelineLayout),
    compile: &metal2vulkan::device::Device,
    state: &BindState,
    samplers: &mut Samplers<'_>,
) -> Result<NmbDirectKernelWrite, String> {
    let mut out = NmbDirectKernelWrite {
        write: write_of(w, &commands.descriptor_ranges),
        view_format: 0,
        components: [0; 4],
        plane: u32::from(alpha_plane(layout, w.set, w.binding)),
        sampler: 0,
        sampler_state: NmbDirectSamplerState::default(),
        has_sampler: false,
    };
    match &w.source {
        WriteSource::Texture { view, .. } => (out.view_format, out.components) = view_parts(view),
        WriteSource::TexelBuffer { format, .. } => out.view_format = format.as_raw() as u32,
        WriteSource::Sampler {
            slot,
            compare,
            integer,
            opaque_black,
        } => {
            let source = SamplerSource::Slot {
                slot: *slot,
                compare: *compare,
                integer: *integer,
                opaque_black: *opaque_black,
            };
            out.take_sampler(samplers.make(compile, state, &source, None)?);
        }
        WriteSource::Neutral(neutral @ NeutralResource::Sampler { .. }) => {
            out.take_sampler(samplers.make(compile, state, &SamplerSource::Neutral(*neutral), None)?);
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            let format = match image {
                ImageSource::Texture { view, .. } => {
                    (out.view_format, out.components) = view_parts(view);
                    Some(view.format)
                }
                ImageSource::Neutral(_) => None,
            };
            if out.write.sampler != NMB_DIRECT_IMMUTABLE {
                out.take_sampler(samplers.make(compile, state, sampler, format)?);
            }
        }
        _ => {}
    }
    Ok(out)
}
