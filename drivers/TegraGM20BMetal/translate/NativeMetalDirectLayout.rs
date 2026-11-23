// SPDX-License-Identifier: MIT

use crate::descriptors::e;
use crate::direct_abi::{
    NmbDirectBinding, NmbDirectDraw, NmbDirectLayout, NmbDirectPushRange, NmbDirectSamplerState, NmbDirectSet,
};
use ash::vk as avk;
use metal2vulkan::bind::heap_class_code;
use metal2vulkan::pipeline::{BindingSource, PipelineLayout, RenderPipeline};

/// What vk_sampler_state_init reads of `info`, with the reduction mode and border mapping metal2vulkan chains to it
pub fn sampler_state(
    info: &avk::SamplerCreateInfo<'_>,
    reduction: avk::SamplerReductionMode,
    components: Option<avk::ComponentMapping>,
) -> NmbDirectSamplerState {
    let c = components.unwrap_or_default();
    NmbDirectSamplerState {
        mag_filter: info.mag_filter.as_raw() as u32,
        min_filter: info.min_filter.as_raw() as u32,
        mipmap_mode: info.mipmap_mode.as_raw() as u32,
        address: [info.address_mode_u, info.address_mode_v, info.address_mode_w].map(|a| a.as_raw() as u32),
        mip_lod_bias: info.mip_lod_bias,
        min_lod: info.min_lod,
        max_lod: info.max_lod,
        max_anisotropy: info.max_anisotropy,
        compare_op: if info.compare_enable != 0 {
            info.compare_op.as_raw() as u32
        } else {
            0
        },
        border_color: info.border_color.as_raw() as u32,
        reduction: reduction.as_raw() as u32,
        border_components: [c.r, c.g, c.b, c.a].map(|s| s.as_raw() as u32),
        anisotropy: info.anisotropy_enable != 0,
        compare: info.compare_enable != 0,
        unnormalized: info.unnormalized_coordinates != 0,
    }
}

/// The variant's set-1 heap: its set layout on the runtime's device (0 with none) and each array's class word, by
/// binding
pub(crate) fn heap_of(pipeline: &RenderPipeline, layout: &Layout<'_>, draw: &mut NmbDirectDraw) {
    let Some((at, set)) = pipeline.layout.sets.iter().enumerate().find(|(_, s)| s.set == 1) else {
        return;
    };
    draw.set1 = layout.sets.get(at).map_or(0, |s| avk::Handle::as_raw(*s));
    draw.heap_arrays = set.bindings.len() as u32;
    for b in &set.bindings {
        let code = match &b.source {
            BindingSource::Heap(class) => heap_class_code(class).unwrap_or(0),
            _ => 0,
        };
        if let Some(slot) = draw.heap_class.get_mut(b.binding as usize) {
            *slot = code;
        }
    }
}

/// A variant's layout as data, and as the runtime makes it on its device where there is one (pipeline/objects.rs
/// create_set_layout); with no device its handles are 0
pub struct Layout<'d> {
    device: Option<&'d ash::Device>,
    samplers: Vec<avk::Sampler>,
    sets: Vec<avk::DescriptorSetLayout>,
    pub pipeline: avk::PipelineLayout,
    /// Set 0's bindings, as data
    pub set0_bindings: Vec<NmbDirectBinding>,
    set_bindings: Vec<Vec<NmbDirectBinding>>,
    set_records: Vec<NmbDirectSet>,
    push_ranges: Vec<NmbDirectPushRange>,
    pub data: NmbDirectLayout,
}

impl<'d> Layout<'d> {
    pub fn new(device: Option<&'d ash::Device>, layout: &PipelineLayout) -> Result<Layout<'d>, String> {
        let mut made = Layout {
            device,
            samplers: Vec::new(),
            sets: Vec::new(),
            pipeline: avk::PipelineLayout::null(),
            set0_bindings: Vec::new(),
            set_bindings: Vec::new(),
            set_records: Vec::new(),
            push_ranges: Vec::new(),
            data: NmbDirectLayout {
                sets: std::ptr::null(),
                set_count: 0,
                push: std::ptr::null(),
                push_count: 0,
            },
        };
        for (set_index, set) in layout.sets.iter().enumerate() {
            let mut immutable: Vec<Vec<avk::Sampler>> = Vec::with_capacity(set.bindings.len());
            let mut data_bindings = Vec::with_capacity(set.bindings.len());
            for binding in &set.bindings {
                let mut data = NmbDirectBinding {
                    binding: binding.binding,
                    ty: avk::DescriptorType::from(binding.ty).as_raw(),
                    count: binding.count,
                    flags: avk::DescriptorBindingFlags::from(binding.flags).as_raw(),
                    immutable: false,
                    sampler: NmbDirectSamplerState::default(),
                };
                let Some(desc) = binding.immutable_sampler else {
                    immutable.push(Vec::new());
                    data_bindings.push(data);
                    continue;
                };
                let mut reduction = avk::SamplerReductionModeCreateInfo::default();
                let mut border = avk::SamplerBorderColorComponentMappingCreateInfoEXT::default();
                let info = metal2vulkan::ash_interop::sampler_create_info(&desc, &mut reduction, &mut border);
                data.immutable = true;
                data.sampler = sampler_state(
                    &info,
                    desc.reduction.into(),
                    desc.border_components.map(avk::ComponentMapping::from),
                );
                data_bindings.push(data);
                let Some(device) = device else {
                    immutable.push(Vec::new());
                    continue;
                };
                // SAFETY: `info` and its chain are live for the call.
                let sampler = unsafe { device.create_sampler(&info, None) }.map_err(e("sampler"))?;
                made.samplers.push(sampler);
                immutable.push(vec![sampler; binding.count as usize]);
            }
            let bindings: Vec<avk::DescriptorSetLayoutBinding<'_>> = set
                .bindings
                .iter()
                .zip(&immutable)
                .map(|(binding, samplers)| {
                    let info = avk::DescriptorSetLayoutBinding::default()
                        .binding(binding.binding)
                        .descriptor_type(binding.ty.into())
                        .descriptor_count(binding.count)
                        .stage_flags(binding.stages.into());
                    if samplers.is_empty() {
                        info
                    } else {
                        info.immutable_samplers(samplers)
                    }
                })
                .collect();
            if set_index == 0 {
                made.set0_bindings = data_bindings.clone();
            }
            made.set_bindings.push(data_bindings);
            let Some(device) = device else {
                continue;
            };
            let flags: Vec<avk::DescriptorBindingFlags> = set.bindings.iter().map(|b| b.flags.into()).collect();
            let mut flags_info = avk::DescriptorSetLayoutBindingFlagsCreateInfo::default().binding_flags(&flags);
            let mut info = avk::DescriptorSetLayoutCreateInfo::default()
                .flags(set.flags.into())
                .bindings(&bindings);
            if flags.iter().any(|f| !f.is_empty()) {
                info = info.push_next(&mut flags_info);
            }
            // SAFETY: `info` and its arrays are live for the call.
            made.sets
                .push(unsafe { device.create_descriptor_set_layout(&info, None) }.map_err(e("set layout"))?);
        }
        made.push_ranges = layout
            .push_constants
            .iter()
            .map(|range| NmbDirectPushRange {
                stages: avk::ShaderStageFlags::from(range.stages).as_raw(),
                offset: range.offset,
                size: range.size,
            })
            .collect();
        made.set_records = layout
            .sets
            .iter()
            .zip(&made.set_bindings)
            .map(|(set, bindings)| NmbDirectSet {
                bindings: bindings.as_ptr(),
                binding_count: bindings.len() as u32,
                flags: avk::DescriptorSetLayoutCreateFlags::from(set.flags).as_raw(),
            })
            .collect();
        made.data = NmbDirectLayout {
            sets: made.set_records.as_ptr(),
            set_count: made.set_records.len() as u32,
            push: made.push_ranges.as_ptr(),
            push_count: made.push_ranges.len() as u32,
        };
        let Some(device) = device else {
            return Ok(made);
        };
        let push: Vec<avk::PushConstantRange> = layout
            .push_constants
            .iter()
            .map(|range| {
                avk::PushConstantRange::default()
                    .stage_flags(range.stages.into())
                    .offset(range.offset)
                    .size(range.size)
            })
            .collect();
        let info = avk::PipelineLayoutCreateInfo::default()
            .set_layouts(&made.sets)
            .push_constant_ranges(&push);
        // SAFETY: as above.
        made.pipeline = unsafe { device.create_pipeline_layout(&info, None) }.map_err(e("pipeline layout"))?;
        Ok(made)
    }

    pub fn set0(&self) -> u64 {
        self.sets.first().map_or(0, |s| avk::Handle::as_raw(*s))
    }

    /// The handle of the `at`th set layout, 0: none
    pub fn set_at(&self, at: usize) -> u64 {
        self.sets.get(at).map_or(0, |s| avk::Handle::as_raw(*s))
    }
}

impl Drop for Layout<'_> {
    fn drop(&mut self) {
        let Some(device) = self.device else {
            return;
        };
        // SAFETY: made here, used by no pipeline or set: the stage compile only reads them.
        unsafe {
            device.destroy_pipeline_layout(self.pipeline, None);
            for &set in &self.sets {
                device.destroy_descriptor_set_layout(set, None);
            }
            for &sampler in &self.samplers {
                device.destroy_sampler(sampler, None);
            }
        }
    }
}
