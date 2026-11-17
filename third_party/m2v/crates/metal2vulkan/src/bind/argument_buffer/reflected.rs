//! The layouts Metal makes of a reflected buffer binding (the parent module's docs): by
//! `newArgumentEncoderWithBufferBinding:`, and a plain buffer's by
//! `newArgumentEncoderWithBufferIndex:`.

use std::sync::Arc;

use super::{
    metal_align, type_size_align, ArgumentBufferLayout, ArgumentLayoutError, ArgumentMember,
    ArgumentType,
};
use crate::air::error::DiagName;
use crate::error::{Error, Feature, Unsupported};
use crate::metal::DataType;
use crate::reflection::{BufferBinding, PointerType, StructType};

impl ArgumentBufferLayout {
    /// The layout `newArgumentEncoderWithBufferBinding:` makes of reflected buffer binding
    /// `binding` (a library's, [`Function::reflection`](crate::library::Function::reflection), or
    /// a pipeline's): from its struct type as the reflection states it, by the argument buffer's
    /// rule or the plain buffer's (the module docs). A binding with no struct type answers
    /// Metal's encoder of no bytes, alignment 1, with or without API validation. Refused, as
    /// `Unsupported` ([`Feature::Reflection`]) since Metal's encoder of them is unmeasured: a
    /// plain buffer's member that is no constant (a pointer, say), a member whose data type no
    /// argument has, and an argument buffer's struct array whose elements take no ids of their
    /// own; an array of arrays, as Metal's encoder creation raises.
    pub fn from_buffer_binding(
        binding: &BufferBinding,
    ) -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
        let Some(st) = binding.buffer_struct_type() else {
            return Ok(ArgumentBufferLayout {
                encoded_length: 0,
                alignment: 1,
                members: Vec::new(),
            });
        };
        if binding
            .buffer_pointer_type()
            .is_some_and(PointerType::element_is_argument_buffer)
        {
            reflected_layout(st, Some(binding.buffer_alignment()), Rule::ArgumentBuffer)
        } else {
            reflected_layout(st, None, Rule::PlainBuffer)
        }
    }

    /// The layout `newArgumentEncoderWithBufferIndex:` makes of a plain buffer, from its reflected
    /// binding: Metal's encoder of no bytes, alignment 1, for one of no struct type, and a struct's
    /// members by [`Rule::PlainIndex`], each at the offset the function's own program reads it at.
    pub(crate) fn from_plain_buffer_index(
        binding: &BufferBinding,
    ) -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
        match binding.buffer_struct_type() {
            Some(st) => reflected_layout(st, None, Rule::PlainIndex),
            None => Ok(ArgumentBufferLayout {
                encoded_length: 0,
                alignment: 1,
                members: Vec::new(),
            }),
        }
    }
}

/// Which of Metal's two rules lays out a reflected struct (the module docs, EVs UNI-reflfix-9 to
/// -14).
#[derive(Clone, Copy, PartialEq, Eq)]
enum Rule {
    /// An argument buffer's: a constant takes its MSL size and alignment, and the encoder the
    /// binding's alignment.
    ArgumentBuffer,
    /// A plain buffer's: every constant takes [`PLAIN_CONSTANT_BYTES`] bytes at that alignment,
    /// and the encoder its members' largest alignment. An array whose `argumentIndexStride` is 0
    /// gives element `k` the ids `k` times the element's span.
    PlainBuffer,
    /// A plain buffer's by `newArgumentEncoderWithBufferIndex:`, as an M4 under macOS 27.2 lays it
    /// out: [`Rule::PlainBuffer`]'s, but an integer or `bool` scalar member (an atomic's too) takes
    /// its own size at alignment 1 and no id (one in an array still takes an id), and a pointer
    /// member [`PLAIN_CONSTANT_BYTES`] at that alignment and its own id.
    PlainIndex,
}

/// The bytes, and the alignment, a plain buffer's encoder gives each constant member whatever its
/// type: Metal's answer on an M3 Max (macOS 26.7) for every scalar, vector, matrix and atomic
/// type measured (EV UNI-reflfix-14-m3). An M4 (macOS 27.0.1, EV UNI-reflfix-14) gives an
/// integer or `bool` scalar its own size aligned to 1 instead, and so never a longer or more
/// aligned encoder: a buffer sized and aligned by this rule holds the encoder under either.
const PLAIN_CONSTANT_BYTES: u64 = 8;

/// One element of a reflected member: what it holds, its bytes and alignment, and the layout a
/// struct element or a pointer to an argument buffer has.
struct ReflectedElement {
    ty: ArgumentType,
    size: u64,
    alignment: u64,
    layout: Option<Arc<ArgumentBufferLayout>>,
    /// The ids one element takes: a struct its members' span, anything else one, but none for
    /// [`Rule::PlainIndex`]'s integer scalars.
    ids: u32,
    /// The ids an array of the element steps by where its `argumentIndexStride` is 0: `ids`, but
    /// for [`Rule::PlainIndex`]'s struct the ids its last member reaches.
    step: u32,
}

/// A reflected struct's members by a rule, the end of its members, the end of its last member,
/// its members' largest alignment, and the ids its last member reaches.
struct ReflectedMembers {
    members: Vec<ArgumentMember>,
    end: u64,
    last_end: u64,
    alignment: u64,
    last_span: u32,
}

/// Struct `st` as an encoder's arguments by `rule`, aligned to `alignment` (its members' largest
/// alignment when `None`): its members, and the end of its last element rounded up to the
/// alignment.
fn reflected_layout(
    st: &StructType,
    alignment: Option<u64>,
    rule: Rule,
) -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
    let r = reflected_members(st, rule)?;
    let alignment = alignment.unwrap_or(r.alignment);
    let end = match rule {
        Rule::PlainIndex => r.last_end,
        _ => r.end,
    };
    let encoded_length = metal_align(end, alignment).ok_or(ArgumentLayoutError::TooLarge)?;
    Ok(ArgumentBufferLayout {
        encoded_length,
        alignment,
        members: r.members,
    })
}

/// The members of reflected struct `st` by `rule` ([`ReflectedMembers`]). Under
/// [`Rule::PlainIndex`] an array of structs whose `argumentIndexStride` is 0 steps by the ids the
/// struct's last member reaches, as an M4 does: a struct whose last member takes no id gives every
/// element the same ids, and the last element's win. The recursion is as deep as the struct type,
/// which the reflection built from a decoded layout under the nesting cap.
fn reflected_members(st: &StructType, rule: Rule) -> Result<ReflectedMembers, ArgumentLayoutError> {
    let mut members = Vec::with_capacity(st.members().len());
    let (mut end, mut last_end, mut alignment, mut last_span) = (0u64, 0u64, 1u64, 0u32);
    for m in st.members() {
        let id = m.argument_index();
        let (element, count, stride, ids_per_element) = match m.array_type() {
            Some(a) => {
                if a.element_array_type().is_some() {
                    return Err(ArgumentLayoutError::ArrayOfArrays { id });
                }
                let element = reflected_element(
                    m.name(),
                    a.element_type(),
                    a.element_struct_type(),
                    a.element_pointer_type(),
                    rule,
                    true,
                )?;
                let ids = match (a.argument_index_stride(), rule) {
                    (0, Rule::ArgumentBuffer) if element.ty == ArgumentType::Struct => {
                        return Err(unmeasured(m.name()));
                    }
                    (0, _) => element.step,
                    (ids, _) => ids,
                };
                (element, a.array_length(), a.stride(), ids)
            }
            None => {
                let element = reflected_element(
                    m.name(),
                    m.data_type(),
                    m.struct_type(),
                    m.pointer_type(),
                    rule,
                    false,
                )?;
                let ids = element.ids;
                let size = element.size;
                (element, 1, size, ids)
            }
        };
        let last = stride
            .checked_mul(u64::from(count.saturating_sub(1)))
            .and_then(|bytes| bytes.checked_add(m.offset()))
            .and_then(|start| start.checked_add(element.size))
            .ok_or(ArgumentLayoutError::TooLarge)?;
        end = end.max(last);
        last_end = last;
        alignment = alignment.max(element.alignment);
        last_span = id.saturating_add(count.saturating_mul(ids_per_element));
        if ids_per_element > 0 {
            members.push(ArgumentMember {
                id,
                count,
                ids_per_element,
                offset: m.offset(),
                stride,
                size: element.size,
                ty: element.ty,
                layout: element.layout,
            });
            continue;
        }
        let Some(inner) = element.layout.filter(|l| !l.members.is_empty()) else {
            continue;
        };
        members.push(ArgumentMember {
            id,
            count: 1,
            ids_per_element: inner.id_span(),
            offset: last - element.size,
            stride: element.size,
            size: element.size,
            ty: element.ty,
            layout: Some(inner),
        });
    }
    Ok(ReflectedMembers {
        members,
        end,
        last_end,
        alignment,
        last_span,
    })
}

/// One element of reflected type `data_type` (with its struct or pointer type) of member `name`,
/// by `rule`, `in_array` where it is an array's element.
fn reflected_element(
    name: &str,
    data_type: DataType,
    st: Option<&StructType>,
    pointer: Option<&PointerType>,
    rule: Rule,
    in_array: bool,
) -> Result<ReflectedElement, ArgumentLayoutError> {
    if let Some(st) = st {
        let r = reflected_members(st, rule)?;
        // An M4 ends a plain buffer's struct at its last member (a union's last, an integer, or
        // a half after a float, can end before an earlier member does), a nested one unpadded.
        let size = match rule {
            Rule::PlainIndex => r.last_end,
            _ => metal_align(r.end, r.alignment).ok_or(ArgumentLayoutError::TooLarge)?,
        };
        let layout = ArgumentBufferLayout {
            encoded_length: size,
            alignment: 1,
            members: r.members,
        };
        let ids = layout.id_span();
        let step = match rule {
            Rule::PlainIndex => r.last_span,
            _ => ids,
        };
        return Ok(ReflectedElement {
            ty: ArgumentType::Struct,
            size,
            alignment: r.alignment,
            layout: Some(Arc::new(layout)),
            ids,
            step,
        });
    }
    let (size, alignment, ty) = match type_size_align(data_type) {
        Some(t) => t,
        // A one-component vector (`packed_float1`), the one member Metal reflects with no data
        // type: an M4 lays it out as any other non-integer constant.
        None if data_type == DataType::None && rule == Rule::PlainIndex => (
            PLAIN_CONSTANT_BYTES,
            PLAIN_CONSTANT_BYTES,
            ArgumentType::Constant,
        ),
        None => return Err(unmeasured(name)),
    };
    match rule {
        Rule::ArgumentBuffer => {
            let nested = match pointer {
                Some(p) if ty == ArgumentType::Buffer && p.element_is_argument_buffer() => p
                    .element_struct_type()
                    .map(|st| {
                        reflected_layout(st, Some(p.alignment()), Rule::ArgumentBuffer)
                            .map(Arc::new)
                    })
                    .transpose()?,
                _ => None,
            };
            Ok(ReflectedElement {
                ty,
                size,
                alignment,
                layout: nested,
                ids: 1,
                step: 1,
            })
        }
        Rule::PlainIndex
            if ty == ArgumentType::Constant && !in_array && integer_scalar(data_type) =>
        {
            Ok(ReflectedElement {
                ty,
                size,
                alignment: 1,
                layout: None,
                ids: 0,
                step: 0,
            })
        }
        Rule::PlainBuffer | Rule::PlainIndex if ty == ArgumentType::Constant => {
            Ok(ReflectedElement {
                ty,
                size: PLAIN_CONSTANT_BYTES,
                alignment: PLAIN_CONSTANT_BYTES,
                layout: None,
                ids: 1,
                step: 1,
            })
        }
        Rule::PlainIndex if ty == ArgumentType::Buffer => Ok(ReflectedElement {
            ty,
            size: PLAIN_CONSTANT_BYTES,
            alignment: PLAIN_CONSTANT_BYTES,
            layout: None,
            ids: 1,
            step: 1,
        }),
        Rule::PlainBuffer | Rule::PlainIndex => Err(unmeasured(name)),
    }
}

/// An integer or `bool` scalar, which [`Rule::PlainIndex`] lays out at its own size
fn integer_scalar(ty: DataType) -> bool {
    use DataType as D;
    matches!(
        ty,
        D::Char | D::UChar | D::Bool | D::Short | D::UShort | D::Int | D::UInt | D::Long | D::ULong
    )
}

/// The refusal of a reflected member whose encoder Metal's answer was not measured for.
fn unmeasured(name: &str) -> ArgumentLayoutError {
    ArgumentLayoutError::Compile(Error::Unsupported(
        Unsupported::not_implemented(Feature::Reflection).naming(DiagName::new(name)),
    ))
}
