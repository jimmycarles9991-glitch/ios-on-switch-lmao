//! Flat runs of an argument-buffer layout (`bind::argument_runs`) place every index where the
//! layout's own lookup places it: a function's layouts, and struct arrays of arrays.

use std::sync::Arc;

use super::argument_buffers::function_layout;
use crate::bind::{flat_layouts, ArgumentBufferLayout, ArgumentMember, ArgumentType, FlatLayout};

/// Every index below the layout's span and a few past it, nested layouts too.
fn same_places(layout: &ArgumentBufferLayout, flat: &[FlatLayout], at: usize) {
    let mine = &flat[at];
    assert_eq!(
        (mine.encoded_length, mine.alignment),
        (layout.encoded_length, layout.alignment)
    );
    for index in 0..layout.id_span() + 8 {
        let want = layout.argument(index);
        let got = mine.argument(index);
        match (&want, got) {
            (None, None) => {}
            (Some(p), Some((run, offset))) => {
                assert_eq!(
                    (p.ty, p.offset, p.size),
                    (run.ty, offset, run.size),
                    "index {index}"
                );
                match (&p.nested, run.nested) {
                    (None, None) => {}
                    (Some(n), Some(k)) => same_places(n, flat, k),
                    _ => panic!("index {index}: a nested layout on one side only"),
                }
            }
            _ => panic!("index {index}: {want:?} against {got:?}"),
        }
    }
}

fn check(layout: &ArgumentBufferLayout) {
    let flat = flat_layouts(layout);
    same_places(layout, &flat, 0);
}

#[test]
fn a_functions_runs_place_every_index_as_its_layout_does() {
    for (kernel, index) in [("kA", 0), ("kH", 2)] {
        let Some(layout) = function_layout(kernel, index) else {
            return;
        };
        check(&layout.expect("an argument buffer"));
    }
}

fn member(
    id: u32,
    count: u32,
    per: u32,
    offset: u64,
    stride: u64,
    ty: ArgumentType,
) -> ArgumentMember {
    ArgumentMember {
        id,
        count,
        ids_per_element: per,
        offset,
        stride,
        size: if ty == ArgumentType::Struct {
            stride
        } else {
            8
        },
        ty,
        layout: None,
    }
}

fn of(members: Vec<ArgumentMember>, encoded_length: u64) -> ArgumentBufferLayout {
    ArgumentBufferLayout {
        encoded_length,
        alignment: 8,
        members,
    }
}

#[test]
fn struct_arrays_of_arrays_and_structs_are_runs_too() {
    let leaf = of(
        vec![
            member(0, 1, 1, 0, 8, ArgumentType::Sampler),
            member(1, 2, 1, 8, 8, ArgumentType::Texture),
        ],
        24,
    );
    let mut pair = member(2, 2, 3, 8, 24, ArgumentType::Struct);
    pair.layout = Some(Arc::new(leaf.clone()));
    let inner = of(vec![member(0, 1, 1, 0, 8, ArgumentType::Buffer), pair], 56);
    let mut outer = member(10, 3, 9, 16, 56, ArgumentType::Struct);
    outer.layout = Some(Arc::new(inner));
    let mut pointer = member(40, 2, 1, 184, 8, ArgumentType::Buffer);
    pointer.layout = Some(Arc::new(leaf));
    let layout = of(
        vec![
            member(0, 2, 1, 0, 8, ArgumentType::Texture),
            outer,
            pointer,
            member(12, 1, 1, 200, 8, ArgumentType::Constant),
        ],
        208,
    );
    check(&layout);
    let flat = flat_layouts(&layout);
    assert_eq!(flat.len(), 2, "one nested layout, named once");
    assert_eq!(
        flat[0].argument(12).map(|(r, o)| (r.ty, o)),
        Some((ArgumentType::Constant, 200))
    );
}
