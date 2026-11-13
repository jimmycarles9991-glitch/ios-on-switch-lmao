//! An [`ArgumentBufferLayout`] as flat runs of leaf arguments, for an encoder outside the
//! translator that places each argument by plain arithmetic: [`flat_layouts`] gives a layout and
//! every argument buffer its pointers point to, each a list of [`ArgumentRun`]s whose last run
//! holding an index places it, as [`ArgumentBufferLayout::argument`] places it.
//!
//! A run covers, from its first id, `count` elements of `ids_per_element` ids each, of which the
//! first `width` are arguments: element `e`'s argument `w` sits at
//! `offset + e · stride + w · element_stride`. A member that is no struct is one run of width 1; a
//! struct member's runs are its struct's, each spread over the member's elements, and a run that
//! is itself spread is repeated per element of a struct array that holds it.

use std::sync::Arc;

use super::argument_buffer::{ArgumentBufferLayout, ArgumentType};

/// One run of leaf arguments (the module docs).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ArgumentRun {
    pub id: u32,
    pub count: u32,
    pub ids_per_element: u32,
    pub width: u32,
    pub offset: u64,
    pub stride: u64,
    pub element_stride: u64,
    /// Bytes of one argument.
    pub size: u64,
    pub ty: ArgumentType,
    /// The index in [`flat_layouts`]' list of the argument buffer a `Buffer` points to.
    pub nested: Option<usize>,
}

impl ArgumentRun {
    /// Where index `index` lands in this run: its byte offset.
    pub fn place(&self, index: u32) -> Option<u64> {
        let rel = index.checked_sub(self.id)?;
        let per = self.ids_per_element.max(1);
        let (element, within) = (rel / per, rel % per);
        if element >= self.count || within >= self.width {
            return None;
        }
        self.offset
            .checked_add(self.stride.checked_mul(u64::from(element))?)?
            .checked_add(self.element_stride.checked_mul(u64::from(within))?)
    }
}

/// One argument buffer's length, alignment and runs.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FlatLayout {
    pub encoded_length: u64,
    pub alignment: u64,
    pub runs: Vec<ArgumentRun>,
}

impl FlatLayout {
    /// Where index `index` lands: the last run that holds it, its type and byte offset.
    pub fn argument(&self, index: u32) -> Option<(&ArgumentRun, u64)> {
        self.runs
            .iter()
            .rev()
            .find_map(|run| run.place(index).map(|offset| (run, offset)))
    }
}

/// `layout` flat (index 0), then each argument buffer a pointer of it, or of one before, points to,
/// once per layout.
pub fn flat_layouts(layout: &ArgumentBufferLayout) -> Vec<FlatLayout> {
    let mut out = Vec::new();
    let mut seen: Vec<*const ArgumentBufferLayout> = Vec::new();
    flatten_into(layout, &mut out, &mut seen);
    out
}

fn flatten_into(
    layout: &ArgumentBufferLayout,
    out: &mut Vec<FlatLayout>,
    seen: &mut Vec<*const ArgumentBufferLayout>,
) -> usize {
    let at = out.len();
    out.push(FlatLayout {
        encoded_length: layout.encoded_length,
        alignment: layout.alignment,
        runs: Vec::new(),
    });
    let runs = runs_of(layout, out, seen);
    out[at].runs = runs;
    at
}

fn nested_index(
    nested: &Arc<ArgumentBufferLayout>,
    out: &mut Vec<FlatLayout>,
    seen: &mut Vec<*const ArgumentBufferLayout>,
) -> usize {
    let key = Arc::as_ptr(nested);
    if let Some(at) = seen.iter().position(|p| *p == key) {
        return at + 1;
    }
    seen.push(key);
    let named = seen.len();
    let at = flatten_into(nested, out, seen);
    debug_assert_eq!(at, named);
    at
}

/// The runs of `layout`'s members, in member order.
fn runs_of(
    layout: &ArgumentBufferLayout,
    out: &mut Vec<FlatLayout>,
    seen: &mut Vec<*const ArgumentBufferLayout>,
) -> Vec<ArgumentRun> {
    let mut runs = Vec::new();
    for m in &layout.members {
        let per = m.ids_per_element.max(1);
        if m.ty != ArgumentType::Struct {
            let nested = match (&m.layout, m.ty) {
                (Some(l), ArgumentType::Buffer) => Some(nested_index(l, out, seen)),
                _ => None,
            };
            runs.push(ArgumentRun {
                id: m.id,
                count: m.count,
                ids_per_element: per,
                width: 1,
                offset: m.offset,
                stride: m.stride,
                element_stride: 0,
                size: m.size,
                ty: m.ty,
                nested,
            });
            continue;
        }
        let Some(inner) = &m.layout else {
            continue;
        };
        for r in runs_of(inner, out, seen) {
            let plain = r.ids_per_element == 1 && r.width == 1;
            if plain {
                runs.push(ArgumentRun {
                    id: m.id + r.id,
                    count: m.count,
                    ids_per_element: per,
                    width: r.count,
                    offset: m.offset + r.offset,
                    stride: m.stride,
                    element_stride: r.stride,
                    ..r
                });
                continue;
            }
            for e in 0..m.count {
                runs.push(ArgumentRun {
                    id: m.id + e * per + r.id,
                    offset: m.offset + u64::from(e) * m.stride + r.offset,
                    ..r.clone()
                });
            }
        }
    }
    runs
}
