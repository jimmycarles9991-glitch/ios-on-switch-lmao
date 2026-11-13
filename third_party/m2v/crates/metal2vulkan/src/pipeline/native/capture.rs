use super::{NativeProgram, NativeRecord};
use crate::budget::Meter;
use crate::error::Error;
use crate::pipeline::sym::Symbol;
use crate::spirv::ids::{ConstId, TypeId};
use crate::spirv::ir::*;
use crate::spv::Op;

mod instruction;
mod image;

const NONE: u32 = u32::MAX;

struct Capture<'a> {
    module: &'a TypedModule,
    meter: &'a mut Meter,
    program: NativeProgram,
}

impl Capture<'_> {
    fn constant(&self, id: ConstId) -> Result<u32, Error> {
        Ok(self.module.const_value(id)?.0)
    }

    fn optional_constant(&self, id: Option<ConstId>) -> Result<u32, Error> {
        id.map(|c| self.constant(c))
            .transpose()
            .map(|v| v.unwrap_or(NONE))
    }

    fn emit(
        &mut self,
        kind: u32,
        opcode: Op,
        result: u32,
        ty: u32,
        flags: u32,
        auxiliary: u32,
        operands: impl IntoIterator<Item = u32>,
    ) -> Result<usize, Error> {
        self.program.record(
            self.meter,
            NativeRecord {
                kind,
                opcode: opcode as u32,
                result,
                ty,
                flags,
                auxiliary,
                ..NativeRecord::default()
            },
            operands,
        )
    }

    fn ty(&mut self, id: TypeId) -> Result<bool, Error> {
        let ty = self.module.types().get(id)?;
        let emit = |this: &mut Self, op, flags, args: &[u32]| {
            this.emit(0, op, id.0, NONE, flags, 0, args.iter().copied())
                .map(|_| true)
        };
        match ty {
            Type::Void => emit(self, Op::TypeVoid, 0, &[]),
            Type::Bool => emit(self, Op::TypeBool, 0, &[]),
            Type::Int { width, signed } => emit(
                self,
                Op::TypeInt,
                0,
                &[u32::from(*width), u32::from(*signed)],
            ),
            Type::Float { width } => emit(self, Op::TypeFloat, 0, &[u32::from(*width)]),
            Type::Vector { elem, len } => emit(self, Op::TypeVector, 0, &[elem.0, u32::from(*len)]),
            Type::Array { elem, len, stride } => emit(
                self,
                Op::TypeArray,
                stride.unwrap_or(0),
                &[elem.0, self.constant(*len)?],
            ),
            Type::RuntimeArray { elem, stride } => {
                emit(self, Op::TypeRuntimeArray, stride.unwrap_or(0), &[elem.0])
            }
            Type::Struct { members, layout } => {
                if layout
                    .as_ref()
                    .is_some_and(|l| l.members.iter().any(|m| m.matrix_stride.is_some()))
                {
                    return Ok(false);
                }
                let flags = u32::from(layout.as_ref().is_some_and(|l| l.block));
                let args = members.iter().enumerate().flat_map(|(i, t)| {
                    [t.0, layout.as_ref().map_or(NONE, |l| l.members[i].offset)]
                });
                self.emit(0, Op::TypeStruct, id.0, NONE, flags, 0, args)?;
                Ok(true)
            }
            Type::Pointer { class, pointee } => emit(
                self,
                Op::TypePointer,
                0,
                &[class.to_spirv() as u32, pointee.0],
            ),
            Type::Function { ret, params } if params.is_empty() => {
                emit(self, Op::TypeFunction, 0, &[ret.0])
            }
            Type::Image(image) => {
                let dim = match image.dim {
                    Dim::D1 => 0, Dim::D2 => 1, Dim::D3 => 2,
                    Dim::Cube => 3, Dim::Buffer => 5,
                    _ => return Ok(false),
                };
                emit(self, Op::TypeImage, 0, &[image.sampled_type.0, dim,
                    u32::from(image.depth), u32::from(image.arrayed),
                    u32::from(image.multisampled),
                    if image.usage == ImageUsage::Sampled { 1 } else { 2 },
                    image.format as u32])
            }
            Type::Sampler => emit(self, Op::TypeSampler, 0, &[]),
            Type::SampledImage { image } => emit(self, Op::TypeSampledImage, 0, &[image.0]),
            _ => Ok(false),
        }
    }

    fn const_decl(&mut self, id: ConstId) -> Result<bool, Error> {
        let d = self.module.consts().get(id)?;
        let result = self.constant(id)?;
        let (op, auxiliary) = match &d.kind {
            ConstKind::Plain(Constant::Scalar { bits, .. }) => {
                self.emit(
                    1,
                    Op::Constant,
                    result,
                    d.ty.0,
                    0,
                    0,
                    [bits.0 as u32, (bits.0 >> 32) as u32],
                )?;
                return Ok(true);
            }
            ConstKind::Plain(Constant::Null { .. }) => {
                self.emit(1, Op::ConstantNull, result, d.ty.0, 0, 0, [])?;
                return Ok(true);
            }
            ConstKind::Spec { key, default } => {
                let at = self.emit(
                    1,
                    Op::SpecConstant,
                    result,
                    d.ty.0,
                    0,
                    0,
                    [default.0 as u32, (default.0 >> 32) as u32, 0],
                )?;
                self.program.symbol(
                    self.meter,
                    self.program.records[at].first as usize + 2,
                    Symbol::SpecId(*key),
                )?;
                return Ok(true);
            }
            ConstKind::Plain(Constant::Composite { .. }) => (Op::ConstantComposite, 0),
            ConstKind::WorkgroupSize { .. } => (Op::ConstantComposite, 1),
            ConstKind::SpecOp { op, .. } => (
                Op::SpecConstantOperation,
                match op {
                    SpecOp::IAdd => Op::IAdd,
                    SpecOp::IMul => Op::IMul,
                    SpecOp::BitwiseAnd => Op::BitwiseAnd,
                    SpecOp::ShiftRightLogical => Op::ShiftRightLogical,
                    SpecOp::ULessThan => Op::ULessThan,
                    SpecOp::Select => Op::Select,
                } as u32,
            ),
        };
        let args: &[ConstId] = match &d.kind {
            ConstKind::Plain(Constant::Composite { parts, .. }) => parts,
            ConstKind::WorkgroupSize { parts } => parts,
            ConstKind::SpecOp { operands, .. } => operands,
            _ => unreachable!(),
        };
        // Constant IDs and SSA IDs are distinct arenas, even where their indices happen to match.
        for c in args {
            self.module.const_value(*c)?;
        }
        let args = args.iter().map(|c| self.module.const_value(*c).unwrap().0);
        self.program.record(
            self.meter,
            NativeRecord {
                kind: 1,
                opcode: op as u32,
                result,
                ty: d.ty.0,
                auxiliary,
                ..NativeRecord::default()
            },
            args,
        )?;
        Ok(true)
    }

    fn global(&mut self, id: crate::spirv::ids::GlobalId) -> Result<bool, Error> {
        let g = &self.module.globals()[id.index()];
        let (kind, a, b) = match &g.kind {
            GlobalKind::Resource(_) => (0, 0, 0),
            GlobalKind::PushBlock => (1, 0, 0),
            GlobalKind::BuiltIn {
                builtin,
                dir: IoDir::Input,
            } => (2, *builtin as u32, 0),
            GlobalKind::Plain => (3, 0, 0),
            _ => return Ok(false),
        };
        if !matches!(
            g.class,
            StorageClass::Uniform
                | StorageClass::UniformConstant
                | StorageClass::StorageBuffer
                | StorageClass::PushConstant
                | StorageClass::Input
                | StorageClass::Private
                | StorageClass::Workgroup
        ) {
            return Ok(false);
        }
        let at = self.emit(
            2,
            Op::Variable,
            g.value.0,
            g.ptr_ty.0,
            0,
            0,
            [
                g.pointee.0,
                g.class.to_spirv() as u32,
                self.optional_constant(g.init)?,
                kind,
                a,
                b,
            ],
        )?;
        if let GlobalKind::Resource(slot) = &g.kind {
            let first = self.program.records[at].first as usize;
            self.program
                .symbol(self.meter, first + 4, Symbol::DescriptorSet(*slot))?;
            self.program
                .symbol(self.meter, first + 5, Symbol::Binding(*slot))?;
        }
        Ok(true)
    }

    fn run(&mut self) -> Result<bool, Error> {
        let entry = self.module.entry();
        if entry.model != ExecModel::GLCompute || self.module.functions().len() != 1 {
            return Ok(false);
        }
        let function = &self.module.functions()[entry.func.index()];
        if !function.params.is_empty() || self.module.types().get(function.ret)? != &Type::Void {
            return Ok(false);
        }
        self.program.entry = function.blocks[0].0;
        for mode in &entry.modes {
            match mode {
                ExecMode::LocalSize { x, y, z } => {
                    self.emit(8, Op::ExecutionMode, 0, 0, 0, 0, [*x, *y, *z])?;
                }
                ExecMode::SignedZeroInfNanPreserve { width } => {
                    self.emit(8, Op::ExecutionMode, 1, 0, u32::from(*width), 0, [])?;
                }
                ExecMode::MaximallyReconverges => {
                    self.emit(8, Op::ExecutionMode, 2, 0, 0, 0, [])?;
                }
                ExecMode::SubgroupUniformControlFlow => {
                    self.emit(8, Op::ExecutionMode, 3, 0, 0, 0, [])?;
                }
                _ => return Ok(false),
            }
        }
        for decl in self.module.decls() {
            self.meter.tick(1)?;
            let accepted = match *decl {
                Decl::Type(id) => self.ty(id)?,
                Decl::Const(id) => self.const_decl(id)?,
                Decl::Global(id) => self.global(id)?,
            };
            if !accepted {
                return Ok(false);
            }
        }
        for id in &function.vars {
            let var = &self.module.vars()[id.index()];
            self.emit(
                3,
                Op::Variable,
                var.value.0,
                var.ptr_ty.0,
                0,
                0,
                [var.pointee.0, self.optional_constant(var.init)?],
            )?;
        }
        for id in &function.blocks {
            let block = &self.module.blocks()[id.index()];
            self.meter.tick(block.insts.len() as u32 + 1)?;
            self.emit(4, Op::Label, id.0, NONE, 0, 0, [])?;
            for phi in &block.phis {
                self.emit(
                    5,
                    Op::Phi,
                    phi.result.0,
                    self.module.value(phi.result)?.ty.0,
                    0,
                    0,
                    phi.incoming
                        .iter()
                        .flat_map(|(value, block)| [value.0, block.0]),
                )?;
            }
            for node in &block.insts {
                if !self.instruction(node)? {
                    return Ok(false);
                }
            }
            match &block.term {
                Terminator::Branch(to) => {
                    self.emit(7, Op::Branch, NONE, NONE, 0, 0, [to.0])?;
                }
                Terminator::CondBranch { cond, t, f } => {
                    self.emit(
                        7,
                        Op::BranchConditional,
                        NONE,
                        NONE,
                        0,
                        0,
                        [cond.0, t.0, f.0],
                    )?;
                }
                Terminator::Return | Terminator::Unreachable => {
                    self.emit(7, Op::Return, NONE, NONE, 0, 0, [])?;
                }
                _ => return Ok(false),
            }
        }
        Ok(true)
    }
}

pub(crate) fn capture(
    module: &TypedModule,
    meter: &mut Meter,
) -> Result<Option<NativeProgram>, Error> {
    let mut capture = Capture {
        module,
        meter,
        program: NativeProgram {
            records: Vec::new(),
            operands: Vec::new(),
            links: Vec::new(),
            types: module.types().len() as u32,
            values: module.values().len() as u32,
            blocks: module.blocks().len() as u32,
            entry: 0,
        },
    };
    if capture.run()? {
        Ok(Some(capture.program))
    } else {
        capture.meter.release(capture.program.footprint());
        Ok(None)
    }
}
