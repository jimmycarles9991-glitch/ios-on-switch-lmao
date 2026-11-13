use super::{VerifyError, Violation};
use crate::device::NativeShaderTarget;
use crate::spirv::ids::{BlockId, TypeId, Value};
use crate::spirv::ir::{BinOp, Contraction, ConvOp, GlslOp, Inst, Type, TypedModule, UnOp};

fn contains_half(module: &TypedModule, ty: TypeId) -> bool {
    match module.types.get(ty).ok() {
        Some(Type::Float { width: 16 }) => true,
        Some(
            Type::Vector { elem, .. } | Type::Array { elem, .. } | Type::RuntimeArray { elem, .. },
        ) => contains_half(module, *elem),
        Some(Type::Matrix { col, .. }) => contains_half(module, *col),
        Some(Type::Struct { members, .. }) => members.iter().any(|ty| contains_half(module, *ty)),
        Some(Type::Pointer { pointee, .. }) => contains_half(module, *pointee),
        Some(Type::Function { ret, params }) => {
            contains_half(module, *ret) || params.iter().any(|ty| contains_half(module, *ty))
        }
        _ => false,
    }
}

fn scalar_width(module: &TypedModule, ty: TypeId) -> Option<u8> {
    match module.types.get(ty).ok()? {
        Type::Float { width } => Some(*width),
        Type::Vector { elem, len: 1..=4 } => scalar_width(module, *elem),
        _ => None,
    }
}

pub(super) fn check(module: &TypedModule, target: NativeShaderTarget) -> Result<(), VerifyError> {
    if target != NativeShaderTarget::Sm53 {
        return Ok(());
    }
    for (id, ty) in module.types.iter() {
        if contains_half(module, id)
            && !matches!(
                ty,
                Type::Float { width: 16 } | Type::Vector { len: 1..=4, .. }
            )
        {
            return Err(Violation::NativeHalfType(id).into());
        }
    }
    let value_type = |value: Value| module.values[value.index()].ty;
    for (index, block) in module.blocks.iter().enumerate() {
        for node in &block.insts {
            let mut uses_half = node
                .result
                .is_some_and(|value| contains_half(module, value_type(value)));
            node.inst
                .for_each_value(|value| uses_half |= contains_half(module, value_type(value)));
            if !uses_half {
                continue;
            }
            let legal = match &node.inst {
                Inst::Binary {
                    op: BinOp::FAdd | BinOp::FSub | BinOp::FMul,
                    contract: Contraction::Forbidden,
                    ..
                }
                | Inst::Unary {
                    op: UnOp::FNegate | UnOp::CopyObject,
                    ..
                }
                | Inst::Glsl {
                    op: GlslOp::Fma,
                    contract: Contraction::Forbidden,
                    ..
                }
                | Inst::CompositeConstruct { .. }
                | Inst::CompositeExtract { .. }
                | Inst::CompositeInsert { .. }
                | Inst::VectorExtractDynamic { .. }
                | Inst::VectorInsertDynamic { .. }
                | Inst::VectorShuffle { .. }
                | Inst::Select { .. }
                | Inst::Undef => true,
                Inst::Convert {
                    op: ConvOp::FConvert | ConvOp::FConvertRte,
                    a,
                } => {
                    matches!(scalar_width(module, value_type(*a)), Some(16 | 32))
                        && node.result.is_some_and(|value| {
                            matches!(scalar_width(module, value_type(value)), Some(16 | 32))
                        })
                }
                _ => false,
            };
            if !legal {
                return Err(Violation::NativeHalfOperation(BlockId::new(index as u32)).into());
            }
        }
    }
    Ok(())
}
