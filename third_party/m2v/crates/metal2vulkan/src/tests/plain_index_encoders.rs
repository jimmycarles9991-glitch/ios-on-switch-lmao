//! A plain buffer's encoder by `newArgumentEncoderWithBufferIndex:`
//! ([`ArgumentBufferLayout::from_plain_buffer_index`]): every expected length, alignment and
//! offset is what an M4 under macOS 27.2 answered for the same struct (probes of our own MSL; the
//! reflection Metal gave them is built here member by member).

use crate::bind::{ArgumentBufferLayout, ArgumentType};
use crate::metal::DataType as D;
use crate::reflection::{ArrayType, BufferBinding, StructMember, StructType, TypeDetail};

fn plain(name: &str, offset: u64, ty: D) -> StructMember {
    StructMember::new(name.to_owned(), offset, ty, 0, TypeDetail::Plain)
}

fn array(name: &str, offset: u64, element: D, length: u32, stride: u64) -> StructMember {
    let a = ArrayType::new(element, length, stride, 0, TypeDetail::Plain);
    StructMember::new(
        name.to_owned(),
        offset,
        D::Array,
        0,
        TypeDetail::Array(Box::new(a)),
    )
}

fn struct_array(
    name: &str,
    offset: u64,
    element: StructType,
    length: u32,
    stride: u64,
) -> StructMember {
    let a = ArrayType::new(D::Struct, length, stride, 0, TypeDetail::Struct(element));
    StructMember::new(
        name.to_owned(),
        offset,
        D::Array,
        0,
        TypeDetail::Array(Box::new(a)),
    )
}

fn layout(members: Vec<StructMember>) -> ArgumentBufferLayout {
    let binding = BufferBinding::new(4, 0, D::Struct, Some(StructType::new(members)), None);
    ArgumentBufferLayout::from_plain_buffer_index(&binding).expect("a layout")
}

/// `(encodedLength, alignment)` and where ids `0..ids` land (`None`: no argument has the id)
fn answers(l: &ArgumentBufferLayout, ids: u32) -> ((u64, u64), Vec<Option<u64>>) {
    let places = (0..ids)
        .map(|id| l.argument(id).map(|p| p.offset))
        .collect();
    ((l.encoded_length, l.alignment), places)
}

#[test]
fn an_integer_scalar_takes_its_own_size_and_no_id() {
    // struct { float4 a; float b; uint c; bool d; float2 e; short f; uchar3 g; float3 h; }
    let l = layout(vec![
        plain("a", 0, D::Float4),
        plain("b", 16, D::Float),
        plain("c", 20, D::UInt),
        plain("d", 24, D::Bool),
        plain("e", 32, D::Float2),
        plain("f", 40, D::Short),
        plain("g", 44, D::UChar3),
        plain("h", 48, D::Float3),
    ]);
    assert_eq!(answers(&l, 2), ((56, 8), vec![Some(48), None]));
    // struct { ushort s; uint u; }
    let l = layout(vec![plain("s", 0, D::UShort), plain("u", 4, D::UInt)]);
    assert_eq!(answers(&l, 1), ((8, 1), vec![None]));
    // atomic_uint *: a struct of one uint
    let atomic = StructType::new(vec![plain("__s", 0, D::UInt)]);
    let l = layout(vec![StructMember::new(
        "n".to_owned(),
        0,
        D::Struct,
        0,
        TypeDetail::Struct(atomic),
    )]);
    assert_eq!(answers(&l, 1), ((4, 1), vec![None]));
}

#[test]
fn an_array_element_takes_an_id_whatever_its_type() {
    // struct { float m[2][3]; int n[2][2]; float tail; }, flattened as Metal reflects it
    let l = layout(vec![
        array("m", 0, D::Float, 6, 4),
        array("n", 24, D::Int, 4, 4),
        plain("tail", 40, D::Float),
    ]);
    assert_eq!(
        answers(&l, 6),
        (
            (48, 8),
            vec![Some(40), Some(28), Some(32), Some(36), Some(16), Some(20)]
        )
    );
}

#[test]
fn a_struct_arrays_elements_step_by_their_last_members_ids() {
    // struct { struct { float a; int b; } s[4]; }: the last member takes no id, so every element
    // has id 0 and the last one's is placed
    let ab = StructType::new(vec![plain("a", 0, D::Float), plain("b", 4, D::Int)]);
    let l = layout(vec![struct_array("s", 0, ab, 4, 8)]);
    assert_eq!(answers(&l, 2), ((32, 8), vec![Some(24), None]));
    // struct { float2 a; struct { int q; float r; } in[2]; }
    let qr = StructType::new(vec![plain("q", 0, D::Int), plain("r", 4, D::Float)]);
    let l = layout(vec![
        plain("a", 0, D::Float2),
        struct_array("in", 8, qr, 2, 8),
    ]);
    assert_eq!(answers(&l, 3), ((32, 8), vec![Some(12), Some(20), None]));
}

#[test]
fn a_pointer_member_takes_eight_bytes_and_its_own_id() {
    // struct { float m; device float *ptr; }: Metal reflects ptr at argument index 1
    let l = layout(vec![
        plain("m", 0, D::Float),
        StructMember::new("ptr".to_owned(), 8, D::Pointer, 1, TypeDetail::Plain),
    ]);
    assert_eq!(answers(&l, 2), ((16, 8), vec![Some(0), Some(8)]));
    assert_eq!(l.argument(1).map(|p| p.ty), Some(ArgumentType::Buffer));
}

#[test]
fn a_plain_struct_ends_at_its_last_member() {
    // RenderBox's StrokeLinePoint { packed_float2 p; float radius; half alpha; short join; }: the
    // half's 8 bytes would pass the short's end
    let l = layout(vec![
        plain("p", 0, D::Float2),
        plain("radius", 8, D::Float),
        plain("alpha", 12, D::Half),
        plain("join", 14, D::Short),
    ]);
    assert_eq!(answers(&l, 1), ((16, 8), vec![Some(12)]));
    // MPS image statistics' info: four short2 and ushort2, then two unions of a float, an int and a
    // uint, each ending at its uint
    let union = || {
        StructType::new(vec![
            plain("f", 0, D::Float),
            plain("i", 0, D::Int),
            plain("u", 0, D::UInt),
        ])
    };
    let member = |name: &str, offset| {
        StructMember::new(
            name.to_owned(),
            offset,
            D::Struct,
            0,
            TypeDetail::Struct(union()),
        )
    };
    let l = layout(vec![
        plain("source_origin", 0, D::Short2),
        plain("source_size", 4, D::UShort2),
        plain("destination_size", 8, D::UShort2),
        plain("num_pixels_per_thread", 12, D::UShort2),
        member("u_min", 16),
        member("u_max", 20),
    ]);
    assert_eq!(answers(&l, 1), ((24, 8), vec![Some(20)]));
}

#[test]
fn a_one_component_vector_is_a_constant() {
    // struct { packed_float1 a; packed_int1 b; float c; packed_uint1 d[3]; }: Metal reflects
    // each one-component vector with no data type.
    let l = layout(vec![
        plain("a", 0, D::None),
        plain("b", 4, D::None),
        plain("c", 8, D::Float),
        array("d", 12, D::None, 3, 4),
    ]);
    assert_eq!(
        answers(&l, 4),
        ((32, 8), vec![Some(12), Some(16), Some(20), None])
    );
    // struct { float x; packed_int1 a; }
    let l = layout(vec![plain("x", 0, D::Float), plain("a", 4, D::None)]);
    assert_eq!(answers(&l, 1), ((16, 8), vec![Some(4)]));
    // struct { half x; packed_half1 a; }
    let l = layout(vec![plain("x", 0, D::Half), plain("a", 2, D::None)]);
    assert_eq!(answers(&l, 1), ((16, 8), vec![Some(2)]));
}
