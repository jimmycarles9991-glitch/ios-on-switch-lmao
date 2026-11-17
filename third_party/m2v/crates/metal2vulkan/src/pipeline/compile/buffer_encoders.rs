//! `newArgumentEncoderWithBufferIndex:` at every buffer index a function declares, from one decode
//! of its AIR: an argument buffer's layout as [`super::argument_buffer_layout`] makes it, and a
//! plain buffer's as Metal makes it without API validation (which stops it): of no struct type an
//! encoder of no bytes, alignment 1 (an M4 under macOS 27.2 for all 25711 such buffers of its
//! system metallibs), of a struct [`ArgumentBufferLayout::from_plain_buffer_index`], each member
//! where the function's own program reads it. An index with no buffer is absent: Metal asserts
//! there.
//!
//! A function made without constant values has no buffer a function constant gates: Metal asserts
//! there too (an M4 under macOS 27.2, for RealityKit's `recalculateNormals` and a probe's own).
//! Of a function made with them, as `argument_buffer_layout` does per index, only an index a
//! function constant decides waits on specialization: a specialization that fails refuses those
//! indices alone.

use std::collections::BTreeMap;

use super::{guarded, spec_request, with_request, InputsFrom};
use crate::air;
use crate::air::abi::{AbiInt, EntryPoint, ParamBinding, ParamRole, StructInfo};
use crate::bind::argument_buffer::{self, EntryLayout};
use crate::bind::{ArgumentBufferLayout, ArgumentLayoutError, BufferEncoder};
use crate::budget::Meter;
use crate::error::{Error, InternalError, InternalKind};
use crate::library::{Function, FunctionConstantValues};
use crate::lower::select::LowerInputs;
use crate::pipeline::descriptor::{DynamicLibraries, LinkedFunctions};
use crate::reflection::{self, Channel};

/// Every buffer `f` declares, by index, made with the function constants `consts` (`None`: made
/// without constant values); specialized with them, or none, and no linked functions only where a
/// function constant decides a buffer's index, presence or layout.
pub(crate) fn buffer_encoders(
    f: &Function,
    consts: Option<&FunctionConstantValues>,
    m: &mut Meter,
) -> Result<Vec<BufferEncoder>, ArgumentLayoutError> {
    let (linked, libraries) = (LinkedFunctions::default(), DynamicLibraries::default());
    let inputs = LowerInputs::new();
    let none = FunctionConstantValues::defaults();
    let ungated_only = consts.is_none();
    let answer = guarded(|| {
        with_request(
            f,
            consts.unwrap_or(&none),
            (&linked, &libraries),
            InputsFrom::Given(&inputs),
            |r| {
                let mut decoded = air::decode(r.bitcode, m)?;
                if let Some(c) = r.container {
                    decoded.abi.check_container(&decoded.module, c, m)?;
                }
                let req = spec_request(&decoded, r.entry, r.consts)?;
                let unspecialized = {
                    let ep = air::canon::select(&decoded.module, &decoded.abi, &req.entry)?;
                    survey(ep, &decoded.abi.structs, ungated_only, m)
                };
                let unspecialized = match unspecialized {
                    Some(s) if s.pending.is_empty() => return Ok(s.answered),
                    other => other,
                };
                let specialized = air::specialize_linked(decoded, &req, &[], m)
                    .map_err(Error::from)
                    .and_then(|spec| {
                        survey(&spec.ep, &spec.structs, false, m)
                            .filter(|s| s.pending.is_empty())
                            .ok_or_else(|| {
                                Error::Internal(InternalError::new(
                                    InternalKind::Invariant,
                                    "a specialized entry left a buffer to specialization",
                                ))
                            })
                    });
                Ok(match (unspecialized, specialized) {
                    (None, specialized) => specialized?.answered,
                    (Some(s), specialized) => s.settled(specialized),
                })
            },
        )
    });
    answer.map_err(ArgumentLayoutError::Compile)
}

/// An entry's buffers: those answered as it stands, and the indices (whether one holds an argument
/// buffer) a function constant decides.
struct Survey {
    answered: Vec<BufferEncoder>,
    pending: BTreeMap<u32, bool>,
}

impl Survey {
    /// The answers with each pending index the specialized entry's, or its failure
    fn settled(mut self, specialized: Result<Survey, Error>) -> Vec<BufferEncoder> {
        match specialized {
            Ok(spec) => self.answered.extend(
                spec.answered
                    .into_iter()
                    .filter(|b| self.pending.contains_key(&b.index)),
            ),
            Err(e) => {
                self.answered
                    .extend(
                        self.pending
                            .iter()
                            .map(|(&index, &argument_buffer)| BufferEncoder {
                                index,
                                argument_buffer,
                                layout: Err(ArgumentLayoutError::Compile(e.clone())),
                            }),
                    )
            }
        }
        self.answered.sort_by_key(|b| b.index);
        self.answered
    }
}

/// `ep`'s buffers, those a function constant gates left out where `ungated_only`; `None` where a
/// function constant computes a buffer's index, which decides every index.
fn survey(
    ep: &EntryPoint,
    structs: &[StructInfo],
    ungated_only: bool,
    m: &mut Meter,
) -> Option<Survey> {
    let mut declared: BTreeMap<u32, Vec<&ParamBinding>> = BTreeMap::new();
    let mut gated = BTreeMap::new();
    for p in &ep.params {
        let location = match &p.role {
            ParamRole::ArgumentBuffer(b) | ParamRole::Buffer(b) => b.location,
            _ => continue,
        };
        if ungated_only && p.predicate.is_some() {
            continue;
        }
        let AbiInt::Const(index) = location.index else {
            return None;
        };
        declared.entry(index).or_default().push(p);
        *gated.entry(index).or_insert(false) |= p.predicate.is_some();
    }
    let mut s = Survey {
        answered: Vec::with_capacity(declared.len()),
        pending: BTreeMap::new(),
    };
    for (index, params) in declared {
        let argument_buffer = params
            .iter()
            .any(|p| matches!(p.role, ParamRole::ArgumentBuffer(_)));
        let layout = match (params.as_slice(), gated[&index]) {
            ([p], false) => match &p.role {
                ParamRole::ArgumentBuffer(_) => {
                    match argument_buffer::entry_layout(&ep.params, structs, index, m) {
                        EntryLayout::Done(layout) => Some(layout),
                        EntryLayout::Specialize => None,
                    }
                }
                ParamRole::Buffer(b) if b.layout.is_none() => Some(Ok(ArgumentBufferLayout {
                    encoded_length: 0,
                    alignment: 1,
                    members: Vec::new(),
                })),
                _ => Some(plain_layout(ep, structs, p, index)),
            },
            _ => None,
        };
        match layout {
            Some(layout) => s.answered.push(BufferEncoder {
                index,
                argument_buffer,
                layout,
            }),
            None => {
                s.pending.insert(index, argument_buffer);
            }
        }
    }
    Some(s)
}

/// A plain buffer's encoder, from its own library reflection binding
fn plain_layout(
    ep: &EntryPoint,
    structs: &[StructInfo],
    p: &ParamBinding,
    index: u32,
) -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
    reflection::binding(ep, structs, Channel::Library, p)
        .map_err(ArgumentLayoutError::Compile)?
        .as_ref()
        .and_then(|b| b.buffer())
        .ok_or(ArgumentLayoutError::NoArgumentBuffer { index })
        .and_then(ArgumentBufferLayout::from_plain_buffer_index)
}
