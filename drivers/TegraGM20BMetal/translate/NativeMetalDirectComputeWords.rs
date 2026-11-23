// SPDX-License-Identifier: MIT

use crate::direct_compute_abi::NmbDirectKernel;
use crate::direct_layout::Layout;
use crate::direct_writes::UNNAMED_RESIDUAL;
use ash::vk as avk;
use metal2vulkan::bind::{
    BindingPlan, DispatchCommands, FieldLocation, PushHole, UseTarget, WriteSource, heap_class_code,
};
use metal2vulkan::cache::VariantKey;
use metal2vulkan::pipeline::{BindingSource, PipelineLayout, PushField, SlotClass, Stage};

/// The kernel's reads through argument buffers the driver fills (NativeMetalDirectDraw.rs holes_of and
/// NativeMetalDirectLayout.rs heap_of for a draw): its root-table and ExecutionStatus address holes (push word + 1),
/// the others counted, the buffer slots its root table holds an entry for as the bound dispatch names them, and its set
/// 1's heap arrays
pub(crate) fn arguments_of(
    commands: &DispatchCommands,
    bound: &DispatchCommands,
    layout: &PipelineLayout,
    sets: &Layout<'_>,
    kernel: &mut NmbDirectKernel,
) -> Result<(), String> {
    for hole in &commands.push_holes {
        let (word, offset) = match *hole {
            PushHole::RootTableAddress { offset, .. } => (&mut kernel.root_table_word, offset),
            PushHole::ExecutionStatusAddress { offset, .. } => (&mut kernel.status_word, offset),
            _ => {
                kernel.other_holes += 1;
                continue;
            }
        };
        if offset % 4 != 0 || offset > 128 - 8 {
            return Err("an address push hole outside the root push window".into());
        }
        let encoded = (offset / 4 + 1) as u8;
        if *word != 0 && *word != encoded {
            return Err("the kernel's address push hole at two places".into());
        }
        *word = encoded;
    }
    if let Some(table) = &bound.root_table {
        let field = |root: usize, at: usize| {
            table
                .bytes
                .get(32 * root + at..32 * root + at + 8)
                .and_then(|b| b.try_into().ok())
                .map_or(0, u64::from_le_bytes)
        };
        let first = (field(1, 8) as usize).min(table.bytes.len() / 32);
        for root in 2..first {
            if field(root, 0) == 0 {
                continue;
            }
            if root - 2 >= 31 {
                return Err("a root table entry past the buffer slots".into());
            }
            kernel.root_slots |= 1 << (root - 2);
        }
    }
    let Some((at, set)) = layout.sets.iter().enumerate().find(|(_, s)| s.set == 1) else {
        return Ok(());
    };
    kernel.set1 = sets.set_at(at);
    kernel.heap_arrays = set.bindings.len() as u32;
    for b in &set.bindings {
        let code = match &b.source {
            BindingSource::Heap(class) => heap_class_code(class).unwrap_or(0),
            _ => 0,
        };
        if let Some(slot) = kernel.heap_class.get_mut(b.binding as usize) {
            *slot = code;
        }
    }
    Ok(())
}

pub(crate) fn access_masks(plans: [&BindingPlan; 3]) -> [u32; 4] {
    let mut masks = [0u32; 4];
    for use_ in plans.into_iter().flat_map(|p| &p.uses) {
        let UseTarget::Slot(slot) = use_.resource else {
            if use_.access.write {
                masks[1] = u32::MAX;
                masks[3] = u32::MAX;
            }
            continue;
        };
        if slot.stage != Stage::Kernel || slot.index >= 32 {
            continue;
        }
        let base = match slot.class {
            SlotClass::Buffer => 0,
            SlotClass::Texture => 2,
            SlotClass::Sampler => continue,
        };
        masks[base] |= u32::from(use_.access.read) << slot.index;
        masks[base + 1] |= u32::from(use_.access.write) << slot.index;
    }
    masks
}

pub(crate) fn push_words(commands: &DispatchCommands) -> (u32, [u32; 32]) {
    let (mut mask, mut words) = (0u32, [0u32; 32]);
    let Some(region) = commands.regions.first() else {
        return (mask, words);
    };
    for write in &region.push_constants {
        for (k, word) in write.bytes.chunks(4).enumerate() {
            let at = write.offset as usize / 4 + k;
            if at < 32 && word.len() == 4 {
                words[at] = u32::from_le_bytes([word[0], word[1], word[2], word[3]]);
                mask |= 1 << at;
            }
        }
    }
    (mask, words)
}

/// What a write is in the record: its binding, kind, source and slot (not the range, which the offsets move)
pub(crate) fn layout_of(plan: &BindingPlan) -> Vec<(u32, u32, u32, i32, u32)> {
    plan.descriptors
        .iter()
        .map(|w| {
            let source = match &w.source {
                WriteSource::Buffer { slot } | WriteSource::InlineBytes { slot } => slot.index,
                WriteSource::Texture { slot, .. } | WriteSource::Sampler { slot, .. } => slot.index,
                _ => u32::MAX,
            };
            (
                w.set,
                w.binding,
                w.array_element,
                avk::DescriptorType::from(w.ty).as_raw(),
                source,
            )
        })
        .collect()
}

/// What a kernel's plan changes where the probe binds its buffers off 16 bytes (`bound`'s variant, specialization and
/// writes against those of the same buffers bound 16-aligned), None where nothing: a record holds one plan, whose
/// residual words then carry each buffer's offset, so a plan that changes has no record
fn offset_change(
    [aligned, bound]: [&VariantKey; 2],
    respecialized: bool,
    [aligned_layout, bound_layout]: &[Vec<(u32, u32, u32, i32, u32)>; 2],
) -> Option<String> {
    let mut slots: std::collections::BTreeSet<u32> = aligned
        .psb_slots
        .symmetric_difference(&bound.psb_slots)
        .chain(
            aligned
                .storage_view_slots
                .symmetric_difference(&bound.storage_view_slots),
        )
        .filter(|s| s.class == SlotClass::Buffer)
        .map(|s| s.index)
        .collect();
    let apart = |a: &[(u32, u32, u32, i32, u32)], b: &[(u32, u32, u32, i32, u32)]| {
        a.iter().filter(|w| !b.contains(w)).map(|w| w.4).collect::<Vec<_>>()
    };
    slots.extend(
        apart(aligned_layout, bound_layout)
            .into_iter()
            .chain(apart(bound_layout, aligned_layout)),
    );
    slots.remove(&u32::MAX);
    if !slots.is_empty() {
        let named: Vec<String> = slots.iter().map(u32::to_string).collect();
        return Some(format!("buffer slots {}", named.join(", ")));
    }
    if aligned != bound {
        return Some("its variant".into());
    }
    if aligned_layout != bound_layout {
        return Some("the order of its descriptor writes".into());
    }
    respecialized.then(|| "its specialization constants".into())
}

/// The record's residual words, the plain plan's `words` (NativeMetalDirectWrites.rs residual_words), where they hold
/// every buffer's offset: raw binds a buffer entry 16-aligned and adds back only the remainder a residual word names
/// (compute.cpp kernelBufferDescriptor), so a plan the offsets change (offset_change of `variants`, `respecialized`
/// and `layouts`), or a residual placed where no record word names it, is refused
pub(crate) fn record_residuals(
    variants: [&VariantKey; 2],
    respecialized: bool,
    layouts: &[Vec<(u32, u32, u32, i32, u32)>; 2],
    words: [u8; 32],
) -> Result<[u8; 32], String> {
    if let Some(what) = offset_change(variants, respecialized, layouts) {
        return Err(format!(
            "a kernel whose plan changes with its buffers bound off 16 bytes ({what}): no residual word holds their \
             offsets"
        ));
    }
    match words.iter().position(|&w| w == UNNAMED_RESIDUAL) {
        Some(slot) => Err(format!(
            "buffer slot {slot}'s bind offset residual placed past word 30 of its argument table, which no record word \
             names"
        )),
        None => Ok(words),
    }
}

/// A stage-in kernel's push words the raw dispatch writes: its StageInRegion's and each dynamic layout's StageInStride
pub(crate) fn stage_in_words(plan: &BindingPlan) -> Result<(i32, [[u32; 2]; 4]), String> {
    let (mut region, mut strides, mut n) = (-1, [[0u32; 2]; 4], 0);
    for p in &plan.push.fields {
        let FieldLocation::PushWindow { offset } = p.location else {
            continue;
        };
        match p.field {
            PushField::StageInRegion => region = (offset / 4) as i32,
            PushField::StageInStride(index) => {
                if n == strides.len() {
                    return Err("more than four stage-in layouts of a dynamic stride".into());
                }
                strides[n] = [index, offset / 4 + 1];
                n += 1;
            }
            _ => {}
        }
    }
    Ok((region, strides))
}

pub(crate) fn region_word(plan: &BindingPlan) -> i32 {
    plan.push
        .fields
        .iter()
        .find_map(|p| match (p.field, p.location) {
            (PushField::DispatchRegion, FieldLocation::PushWindow { offset }) => Some((offset / 4) as i32),
            _ => None,
        })
        .unwrap_or(-1)
}

#[cfg(test)]
mod tests {
    use super::*;
    use metal2vulkan::pipeline::SlotRef;

    fn buffer(index: u32) -> SlotRef {
        SlotRef {
            stage: Stage::Kernel,
            class: SlotClass::Buffer,
            index,
        }
    }

    // A record holds one plan, and each buffer's residual word carries its offset: a kernel whose plan the probe's
    // buffers bound off 16 bytes change is refused, naming the slots, and one whose plan they leave keeps its words
    #[test]
    fn a_plan_a_buffer_offset_changes_is_named_and_one_it_leaves_is_kept() {
        let plain = VariantKey::new();
        let layout = vec![(0, 0, 0, 7, 2), (0, 1, 0, 6, 5)];
        let mut words = [0u8; 32];
        words[2] = 4;
        words[5] = 40;
        let kept = record_residuals([&plain, &plain], false, &[layout.clone(), layout.clone()], words);
        assert_eq!(kept, Ok(words));
        let mut addressed = VariantKey::new();
        addressed.psb_slots.insert(buffer(5));
        let moved = vec![(0, 0, 0, 7, 2), (0, 1, 0, 7, 3)];
        let refused = record_residuals([&plain, &addressed], false, &[layout.clone(), moved.clone()], words);
        assert!(refused.unwrap_err().contains("(buffer slots 3, 5)"));
        let named = offset_change([&plain, &addressed], false, &[layout.clone(), moved]);
        assert_eq!(named.as_deref(), Some("buffer slots 3, 5"));
        let mut viewed = VariantKey::new();
        viewed.emulated_simdgroups = true;
        let changed = offset_change([&plain, &viewed], false, &[layout.clone(), layout.clone()]);
        assert_eq!(changed.as_deref(), Some("its variant"));
        let reordered: Vec<_> = layout.iter().rev().copied().collect();
        let order = offset_change([&plain, &plain], false, &[layout.clone(), reordered]);
        assert_eq!(order.as_deref(), Some("the order of its descriptor writes"));
        let specialized = offset_change([&plain, &plain], true, &[layout.clone(), layout]);
        assert_eq!(specialized.as_deref(), Some("its specialization constants"));
    }

    #[test]
    fn a_residual_no_record_word_names_is_refused_by_its_slot() {
        let plain = VariantKey::new();
        let layouts = [vec![(0, 0, 0, 7, 3)], vec![(0, 0, 0, 7, 3)]];
        let mut words = [0u8; 32];
        words[0] = 1;
        words[3] = 63;
        assert_eq!(record_residuals([&plain, &plain], false, &layouts, words), Ok(words));
        words[9] = UNNAMED_RESIDUAL;
        let refused = record_residuals([&plain, &plain], false, &layouts, words).unwrap_err();
        assert!(refused.starts_with("buffer slot 9's"), "{refused}");
    }
}
