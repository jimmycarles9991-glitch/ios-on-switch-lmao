// SPDX-License-Identifier: MIT

use crate::direct_abi::{NMB_DIRECT_IMMUTABLE, NMB_DIRECT_NEUTRAL, NmbDirectSource, NmbDirectWrite, stage_number};
use ash::vk as avk;
use metal2vulkan::bind::{
    BindingPlan, DescriptorBytes, DescriptorRange, FieldLocation, ImageSource, NeutralResource, SamplerSource,
    WriteSource,
};
use metal2vulkan::pipeline::{
    BindingSource, PipelineLayout, PushField, SlotClass, SlotRef, Stage, StorageAttachmentView, SyntheticBinding,
};

pub fn slot_parts(slot: &SlotRef) -> (u32, u32) {
    (stage_number(slot.stage), slot.index)
}

fn neutral_view(neutral: &NeutralResource) -> i32 {
    match neutral {
        NeutralResource::Image { view_type, .. } => avk::ImageViewType::from(*view_type).as_raw(),
        _ => -1,
    }
}

pub fn write_of(w: &metal2vulkan::bind::DescriptorWrite, ranges: &[DescriptorRange]) -> NmbDirectWrite {
    let mut out = NmbDirectWrite {
        set: w.set,
        binding: w.binding,
        element: w.array_element,
        ty: avk::DescriptorType::from(w.ty).as_raw(),
        source: NmbDirectSource::Other,
        stage: 0,
        slot: 0,
        sampler: 0,
        range: 0,
        view_type: -1,
        format: 0,
        compare: 0,
    };
    let sampler_of = |s: &SamplerSource| match s {
        SamplerSource::Slot { slot, .. } => slot.index,
        SamplerSource::Immutable => NMB_DIRECT_IMMUTABLE,
        _ => NMB_DIRECT_NEUTRAL,
    };
    let compares = |s: &SamplerSource| match s {
        SamplerSource::Slot { compare, .. } => u32::from(*compare),
        SamplerSource::Neutral(NeutralResource::Sampler { compare }) => u32::from(*compare),
        _ => 0,
    };
    match &w.source {
        WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => {
            out.source = if matches!(w.source, WriteSource::Buffer { .. }) {
                NmbDirectSource::Buffer
            } else {
                NmbDirectSource::Bytes
            };
            (out.stage, out.slot) = slot_parts(slot);
            out.range = ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        WriteSource::Texture { slot, view } => {
            out.source = NmbDirectSource::Texture;
            (out.stage, out.slot) = slot_parts(slot);
            out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
        }
        WriteSource::TexelBuffer { slot, .. } => {
            out.source = NmbDirectSource::Texture;
            (out.stage, out.slot) = slot_parts(slot);
        }
        WriteSource::Sampler { slot, compare, .. } => {
            out.source = NmbDirectSource::Sampler;
            (out.stage, out.slot) = slot_parts(slot);
            out.sampler = slot.index;
            out.compare = u32::from(*compare);
        }
        WriteSource::CombinedImageSampler { image, sampler } => {
            out.source = NmbDirectSource::Combined;
            match image {
                ImageSource::Texture { slot, view } => {
                    (out.stage, out.slot) = slot_parts(slot);
                    out.view_type = avk::ImageViewType::from(view.view_type).as_raw();
                }
                ImageSource::Neutral(neutral) => {
                    out.slot = NMB_DIRECT_NEUTRAL;
                    out.view_type = neutral_view(neutral);
                }
            }
            out.sampler = sampler_of(sampler);
            out.compare = compares(sampler);
        }
        WriteSource::Attachment { color_index } => {
            out.source = NmbDirectSource::Attachment;
            out.slot = *color_index;
        }
        WriteSource::TileAttachment { color_index, view, .. } => {
            out.source = NmbDirectSource::StorageAttachment;
            out.stage = 2;
            out.slot = *color_index;
            out.view_type = match view {
                StorageAttachmentView::Attachment => avk::ImageViewType::TYPE_2D.as_raw(),
                StorageAttachmentView::SamplePlanes { .. } => avk::ImageViewType::TYPE_2D_ARRAY.as_raw(),
            };
        }
        // the shader-side stencil's shadow is the driver's stencil stand-in (shader_test_push.h shadowStencil)
        WriteSource::DepthStandIn | WriteSource::StencilStandIn | WriteSource::ShadowStencil => {
            out.source = if matches!(w.source, WriteSource::DepthStandIn) {
                NmbDirectSource::DepthStandIn
            } else {
                NmbDirectSource::StencilStandIn
            };
            out.stage = 2;
            out.view_type = avk::ImageViewType::TYPE_2D.as_raw();
        }
        WriteSource::Neutral(neutral) => {
            out.source = NmbDirectSource::Neutral;
            out.view_type = neutral_view(neutral);
        }
        WriteSource::ColorWriteTable => {
            out.source = NmbDirectSource::ColorTable;
            out.stage = 2;
        }
        WriteSource::HandleTable { stage, bytes_len } | WriteSource::ArgumentTable { stage, bytes_len } => {
            out.source = if matches!(w.source, WriteSource::ArgumentTable { .. }) {
                NmbDirectSource::ArgumentTable
            } else {
                NmbDirectSource::TextureTable
            };
            out.stage = stage_number(*stage);
            out.range = u64::from(*bytes_len);
        }
        WriteSource::TessellationFactors | WriteSource::PatchIndices { .. } | WriteSource::ControlPointIndices => {
            out.source = match w.source {
                WriteSource::TessellationFactors => NmbDirectSource::TessFactors,
                WriteSource::PatchIndices { .. } => NmbDirectSource::PatchIndices,
                _ => NmbDirectSource::ControlPointIndices,
            };
            out.stage = 1;
            out.range = ranges
                .iter()
                .find(|r| r.set == w.set && r.binding == w.binding)
                .map_or(0, |r| r.range.range);
        }
        _ => {}
    }
    out
}

/// A handle table's write as its binding's synthetic kind names it, not by its size or place: the texture table or the
/// sampler table
pub(crate) fn table_of(mut write: NmbDirectWrite, layout: &PipelineLayout) -> NmbDirectWrite {
    if write.source == NmbDirectSource::Neutral {
        return shadow_of(write, layout);
    }
    if write.source != NmbDirectSource::TextureTable {
        return write;
    }
    let source = layout
        .sets
        .iter()
        .filter(|s| s.set == write.set)
        .flat_map(|s| &s.bindings)
        .find(|b| b.binding == write.binding)
        .map(|b| &b.source);
    write.source = match source {
        Some(BindingSource::Synthetic {
            binding: SyntheticBinding::SamplerTable,
            ..
        }) => NmbDirectSource::SamplerTable,
        Some(BindingSource::Synthetic {
            binding: SyntheticBinding::HandleTable,
            ..
        }) => NmbDirectSource::TextureTable,
        _ => NmbDirectSource::Other,
    };
    write
}

/// The neutral m2v binds at a shader-side stencil's shadow in a pass without a stencil attachment, which the function never
/// reads: the driver's stencil stand-in binding, empty in such a pass
fn shadow_of(mut write: NmbDirectWrite, layout: &PipelineLayout) -> NmbDirectWrite {
    let shadow = layout
        .sets
        .iter()
        .filter(|s| s.set == write.set)
        .flat_map(|s| &s.bindings)
        .find(|b| b.binding == write.binding)
        .is_some_and(|b| {
            matches!(
                b.source,
                BindingSource::Synthetic {
                    binding: SyntheticBinding::ShadowStencil,
                    ..
                }
            )
        });
    if shadow {
        write.source = NmbDirectSource::StencilStandIn;
        write.stage = 2;
        write.view_type = avk::ImageViewType::TYPE_2D.as_raw();
    }
    write
}

/// The words instantiation gave `stage`'s argument table (m2v's push fields past the stage's push window), empty where its
/// placement spilled none
pub fn spill_table(plan: &BindingPlan, bytes: &[DescriptorBytes], stage: Stage) -> Vec<u32> {
    let Some(write) = plan
        .descriptors
        .iter()
        .find(|w| matches!(w.source, WriteSource::ArgumentTable { stage: s, .. } if s == stage))
    else {
        return Vec::new();
    };
    bytes
        .iter()
        .find(|b| b.set == write.set && b.binding == write.binding)
        .map_or_else(Vec::new, |b| {
            b.bytes
                .chunks(4)
                .map(|w| {
                    w.iter()
                        .enumerate()
                        .fold(0u32, |v, (k, byte)| v | u32::from(*byte) << (8 * k))
                })
                .collect()
        })
}

/// A SlotBindOffset residual the plan places where a record holds no word for it (pack.h bufferResidualWord: 6
/// bits), so the direct compile refuses the state (direct.cpp spillTables) where a record without the word would
/// refuse every draw that binds the slot off its base alignment
pub(crate) const UNNAMED_RESIDUAL: u8 = u8::MAX;

/// By buffer slot, the record's word + 1 of each SlotBindOffset residual of `stage` as the plan places it: a root push
/// word (1 to 32), or the stage's argument table word + 33 (33 to 63); UNNAMED_RESIDUAL past them. The probe's codes
/// cannot name a storage residual whose slot shares its code with another.
pub fn residual_words(plan: &BindingPlan, stage: Stage) -> [u8; 32] {
    let mut out = [0u8; 32];
    for field in plan.push.fields.iter().filter(|f| f.stage == stage) {
        let PushField::SlotBindOffset(slot) = field.field else {
            continue;
        };
        if slot.class != SlotClass::Buffer || slot.index >= 32 {
            continue;
        }
        out[slot.index as usize] = match field.location {
            FieldLocation::PushWindow { offset } if offset / 4 < 32 => (offset / 4 + 1) as u8,
            FieldLocation::ArgumentTable { offset } if offset / 4 < 31 => (offset / 4 + 33) as u8,
            _ => UNNAMED_RESIDUAL,
        };
    }
    out
}

/// Set 0's samplers that take no write: a heap image's constexpr sampler (BindingSource::HeapSampler's immutable one), whose
/// descriptor NVK writes into the set from its layout
pub(crate) fn immutable_samplers(layout: &PipelineLayout, written: &[NmbDirectWrite]) -> Vec<NmbDirectWrite> {
    let mut out = Vec::new();
    for b in layout.sets.iter().filter(|s| s.set == 0).flat_map(|s| &s.bindings) {
        let sampler = avk::DescriptorType::from(b.ty) == avk::DescriptorType::SAMPLER;
        if !sampler || b.immutable_sampler.is_none() || written.iter().any(|w| w.binding == b.binding) {
            continue;
        }
        for element in 0..b.count {
            out.push(NmbDirectWrite {
                set: 0,
                binding: b.binding,
                element,
                ty: avk::DescriptorType::SAMPLER.as_raw(),
                source: NmbDirectSource::Sampler,
                stage: 2,
                slot: 0,
                sampler: NMB_DIRECT_IMMUTABLE,
                range: 0,
                view_type: -1,
                format: 0,
                compare: 0,
            });
        }
    }
    out
}
