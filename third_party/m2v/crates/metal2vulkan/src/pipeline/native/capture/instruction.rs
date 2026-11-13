use super::{Capture, NONE};
use crate::error::Error;
use crate::spirv::ir::*;
use crate::spv::Op;

impl Capture<'_> {
    pub(super) fn instruction(&mut self, node: &InstNode) -> Result<bool, Error> {
        let result = node.result.map_or(NONE, |v| v.0);
        let ty = node
            .result
            .map(|v| self.module.value(v).map(|d| d.ty.0))
            .transpose()?
            .unwrap_or(NONE);
        let emit = |this: &mut Self, op, flags, auxiliary, args: &[u32]| {
            this.emit(6, op, result, ty, flags, auxiliary, args.iter().copied())
                .map(|_| true)
        };
        let access = |a: &MemAccess| {
            u32::from(a.volatile)
                | (u32::from(a.nontemporal) << 1)
                | (u32::from(a.non_private) << 2)
                | (u32::from(a.make_visible.is_some()) << 3)
                | (u32::from(a.make_available.is_some()) << 4)
        };
        match &node.inst {
            Inst::Unary { op, a } => emit(self, op.to_spirv(), 0, 0, &[a.0]),
            Inst::Binary { op, a, b, contract } => emit(
                self,
                op.to_spirv(),
                u32::from(*contract == Contraction::Forbidden),
                0,
                &[a.0, b.0],
            ),
            Inst::Convert { op, a } => emit(
                self,
                op.to_spirv(),
                u32::from(*op == ConvOp::FConvertRte),
                0,
                &[a.0],
            ),
            Inst::Select { cond, t, f } => emit(self, Op::Select, 0, 0, &[cond.0, t.0, f.0]),
            Inst::Fma { a, b, c } => emit(self, Op::FmaKHR, 0, 0, &[a.0, b.0, c.0]),
            Inst::Glsl { op, args, contract } => {
                self.emit(
                    6,
                    Op::ExtInst,
                    result,
                    ty,
                    u32::from(*contract == Contraction::Forbidden),
                    op.to_spirv() as u32,
                    args.iter().map(|v| v.0),
                )?;
                Ok(true)
            }
            Inst::ControlBarrier { exec, mem, sem } => emit(
                self,
                Op::ControlBarrier,
                0,
                0,
                &[
                    exec.scope.to_spirv() as u32,
                    mem.scope.to_spirv() as u32,
                    sem.sem.bits(),
                ],
            ),
            Inst::MemoryBarrier { mem, sem } => emit(
                self,
                Op::MemoryBarrier,
                0,
                0,
                &[mem.scope.to_spirv() as u32, sem.sem.bits()],
            ),
            Inst::BitFieldInsert {
                base,
                insert,
                offset,
                count,
            } => emit(
                self,
                Op::BitFieldInsert,
                0,
                0,
                &[base.0, insert.0, offset.0, count.0],
            ),
            Inst::BitFieldExtract {
                signed,
                base,
                offset,
                count,
            } => emit(
                self,
                if *signed {
                    Op::BitFieldSExtract
                } else {
                    Op::BitFieldUExtract
                },
                0,
                0,
                &[base.0, offset.0, count.0],
            ),
            Inst::Load { ptr, access: a } => {
                if a.make_available.is_some() {
                    return Ok(false);
                }
                let mut args = vec![ptr.0];
                if let Some(scope) = a.make_visible {
                    args.push(scope.scope.to_spirv() as u32);
                }
                emit(self, Op::Load, access(a), a.aligned.unwrap_or(0), &args)
            }
            Inst::Store {
                ptr,
                value,
                access: a,
            } => {
                if a.make_visible.is_some() {
                    return Ok(false);
                }
                let mut args = vec![ptr.0, value.0];
                if let Some(scope) = a.make_available {
                    args.push(scope.scope.to_spirv() as u32);
                }
                emit(self, Op::Store, access(a), a.aligned.unwrap_or(0), &args)
            }
            Inst::AccessChain {
                base,
                indices,
                non_uniform: false,
            } => {
                self.emit(
                    6,
                    Op::AccessChain,
                    result,
                    ty,
                    0,
                    0,
                    std::iter::once(base.0).chain(indices.iter().map(|v| v.0)),
                )?;
                Ok(true)
            }
            Inst::ArrayLength { ptr, member } => emit(self, Op::ArrayLength, 0, *member, &[ptr.0]),
            Inst::CompositeConstruct { parts } => {
                self.emit(
                    6,
                    Op::CompositeConstruct,
                    result,
                    ty,
                    0,
                    0,
                    parts.iter().map(|v| v.0),
                )?;
                Ok(true)
            }
            Inst::CompositeExtract { composite, indices } => {
                self.emit(
                    6,
                    Op::CompositeExtract,
                    result,
                    ty,
                    0,
                    0,
                    std::iter::once(composite.0).chain(indices.iter().copied()),
                )?;
                Ok(true)
            }
            Inst::CompositeInsert {
                object,
                composite,
                indices,
            } => {
                self.emit(
                    6,
                    Op::CompositeInsert,
                    result,
                    ty,
                    0,
                    0,
                    [object.0, composite.0]
                        .into_iter()
                        .chain(indices.iter().copied()),
                )?;
                Ok(true)
            }
            Inst::VectorShuffle { a, b, components } => {
                self.emit(
                    6,
                    Op::VectorShuffle,
                    result,
                    ty,
                    0,
                    0,
                    [a.0, b.0].into_iter().chain(components.iter().copied()),
                )?;
                Ok(true)
            }
            Inst::VectorExtractDynamic { vector, index } => {
                emit(self, Op::VectorExtractDynamic, 0, 0, &[vector.0, index.0])
            }
            Inst::VectorInsertDynamic {
                vector,
                component,
                index,
            } => emit(
                self,
                Op::VectorInsertDynamic,
                0,
                0,
                &[vector.0, component.0, index.0],
            ),
            Inst::Undef => emit(self, Op::Undef, 0, 0, &[]),
            _ => self.image_instruction(node, result, ty),
        }
    }
}
