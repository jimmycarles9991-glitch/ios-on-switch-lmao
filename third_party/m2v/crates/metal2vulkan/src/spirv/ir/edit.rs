//! Typed edits of a finished [`TypedModule`] for IR→IR transforms.
//!
//! A transform decides *what* to change from its own analysis; the edit checks that each change
//! names what it claims to (the right opcode on the right variable of the right function) and
//! applies it whole, then re-derives the entry interface and requirements. Everything else, such as
//! dominance, stays the transform's argument and the finalize verifier's check.

use crate::spirv::error::{BResult, BuildError, Dangling, OperandRule};
use crate::spirv::ids::{BlockId, ConstId, FuncId, TypeId, Value, VarId};
use crate::spirv::ir::{
    ConstKind, Decl, Inst, InstNode, Merge, Shape, Type, TypedModule, UnOp, ValueDef, ValueOrigin,
};
use crate::spirv::requirements::{interface, recorded, ModuleView};

/// An instruction position: index `index` of block `block`'s instructions.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct InstAt {
    pub block: BlockId,
    pub index: usize,
}

/// A single-store promotion of one `Function` variable: its only store is removed,
/// each of its loads becomes `OpCopyObject` of the stored value (keeping the load's result, so no
/// use has to be rewritten), and the variable leaves its function.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Promotion {
    /// The function owning the variable.
    pub func: FuncId,
    pub var: VarId,
    /// The variable's one `OpStore`.
    pub store: InstAt,
    /// Every `OpLoad` of the variable.
    pub loads: Vec<InstAt>,
}

impl TypedModule {
    /// Apply `promotions`. Each variable must be a `Function` variable of its function, promoted
    /// once; its store and loads must be an `OpStore`/`OpLoad` through the variable's pointer in
    /// blocks of that function; and its type must be one `OpCopyObject` accepts. The caller
    /// guarantees that each store dominates its loads; the verifier checks the result. Nothing
    /// changes when a check fails. The entry interface and requirements are recorded again once,
    /// from the changed module and the module's own subgroup requirement
    /// (`requirements::recorded`), so the plan's subgroup facts survive the edit.
    pub fn apply_promotions(&mut self, promotions: &[Promotion]) -> BResult<()> {
        let mut vars: Vec<(FuncId, VarId)> = promotions.iter().map(|p| (p.func, p.var)).collect();
        vars.sort_unstable();
        if let Some(w) = vars
            .windows(2)
            .find(|w| w.first().map(|x| x.1) == w.get(1).map(|x| x.1))
        {
            let dup = w.first().map_or(VarId::new(0), |x| x.1);
            return Err(BuildError::Dangling(Dangling::Var(dup)));
        }
        // Check everything first, collecting the stored values.
        let mut stored = Vec::with_capacity(promotions.len());
        for p in promotions {
            let var = self
                .vars
                .get(p.var.index())
                .ok_or(BuildError::Dangling(Dangling::Var(p.var)))?;
            if var.func != p.func {
                return Err(BuildError::Dangling(Dangling::Var(p.var)));
            }
            let facts = self.types.facts(var.pointee)?;
            if !matches!(
                facts.shape,
                Shape::Plain | Shape::Implicit | Shape::Explicit
            ) || facts.runtime_sized
            {
                return Err(operand(crate::spv::Op::CopyObject));
            }
            let value = match &self.inst_in(p.func, p.store)?.inst {
                Inst::Store { ptr, value, .. } if *ptr == var.value => *value,
                _ => return Err(operand(crate::spv::Op::Store)),
            };
            for at in &p.loads {
                match &self.inst_in(p.func, *at)?.inst {
                    Inst::Load { ptr, .. } if *ptr == var.value => {}
                    _ => return Err(operand(crate::spv::Op::Load)),
                }
            }
            stored.push(value);
        }
        // Every promoted variable is still declared by its function.
        let mut i = 0;
        while let Some(&(func, _)) = vars.get(i) {
            let n = vars
                .get(i..)
                .unwrap_or(&[])
                .iter()
                .take_while(|x| x.0 == func)
                .count();
            let group = vars.get(i..i + n).unwrap_or(&[]);
            let f = self
                .functions
                .get(func.index())
                .ok_or(BuildError::Dangling(Dangling::Func(func)))?;
            let listed = f
                .vars
                .iter()
                .filter(|v| group.binary_search(&(func, **v)).is_ok())
                .count();
            if listed != n {
                let missing = group
                    .iter()
                    .find(|(_, v)| !f.vars.contains(v))
                    .map_or(VarId::new(0), |x| x.1);
                return Err(BuildError::Dangling(Dangling::Var(missing)));
            }
            i += n;
        }
        // Loads become copies in place (indices do not move); stores are removed afterwards,
        // one pass per block over sorted indices.
        let mut removals = Vec::with_capacity(promotions.len());
        for (p, value) in promotions.iter().zip(&stored) {
            for at in &p.loads {
                self.inst_mut(*at)?.inst = Inst::Unary {
                    op: UnOp::CopyObject,
                    a: *value,
                };
            }
            removals.push(p.store);
        }
        removals.sort_unstable();
        for group in removals.chunk_by(|a, b| a.block == b.block) {
            let Some(first) = group.first() else {
                continue;
            };
            let block = self
                .blocks
                .get_mut(first.block.index())
                .ok_or(BuildError::Dangling(Dangling::Block(first.block)))?;
            if group.last().is_some_and(|at| at.index >= block.insts.len()) {
                return Err(BuildError::Dangling(Dangling::Block(first.block)));
            }
            remove_at(&mut block.insts, group.iter().map(|at| at.index));
        }
        for (fi, f) in self.functions.iter_mut().enumerate() {
            let func = FuncId::new(u32::try_from(fi).unwrap_or(u32::MAX));
            if vars.binary_search_by_key(&func, |x| x.0).is_ok() {
                f.vars.retain(|v| vars.binary_search(&(func, *v)).is_err());
            }
        }
        self.rederive()
    }

    /// Replace each instruction of `copies` by `OpCopyObject` of its value, then remove the
    /// instructions at `insts`, the phis at `phis` (block, index), the declarations of the
    /// constants `consts` and of the vector and function types `types`, and the functions `funcs`,
    /// each named once. A replaced instruction must be one
    /// [`removable`](crate::spirv::dead::removable) names, with a result of the value's type, and
    /// is not removed. Every removed instruction must have a result and be one
    /// [`removable`](crate::spirv::dead::removable) names or a call of a function
    /// [`pure_functions`](crate::spirv::dead::pure_functions) names pure; every constant must be a
    /// declared one [`removable_const`](crate::spirv::dead::removable_const) names. A removed
    /// constant is no longer interned ([`Consts::forget`](crate::spirv::ir::Consts::forget)), so
    /// the interner never hands out a constant the module no longer declares. A removed function
    /// is not the entry, and no instruction that stays calls it; it leaves with its blocks and
    /// variables, the loops, tangle entries, and switch headers in it, and the ids of the
    /// functions, blocks, and variables that stay are renumbered densely in their order, so the
    /// module serializes as if the function had never been built. Its parameters, variables, and
    /// results keep their values, now of origin [`ValueOrigin::Removed`]. The caller guarantees
    /// that nothing outside the removed set reads a removed result or constant; the verifier and
    /// the serializer check the result. Nothing changes when a check fails. The entry interface
    /// and requirements are recorded again once, as after [`TypedModule::apply_promotions`].
    pub fn remove_dead(
        &mut self,
        insts: &[InstAt],
        phis: &[(BlockId, usize)],
        consts: &[ConstId],
        types: &[TypeId],
        copies: &[(InstAt, Value)],
        funcs: &[FuncId],
    ) -> BResult<()> {
        let mut insts = insts.to_vec();
        insts.sort_unstable();
        let mut phis = phis.to_vec();
        phis.sort_unstable();
        let mut consts = consts.to_vec();
        consts.sort_unstable();
        if insts.windows(2).any(|w| w.first() == w.get(1))
            || phis.windows(2).any(|w| w.first() == w.get(1))
            || consts.windows(2).any(|w| w.first() == w.get(1))
        {
            return Err(operand(crate::spv::Op::Nop));
        }
        let declared = self
            .decls
            .iter()
            .filter(|d| matches!(d, Decl::Const(c) if consts.binary_search(c).is_ok()))
            .count();
        if declared != consts.len() {
            return Err(operand(crate::spv::Op::Constant));
        }
        for c in &consts {
            if !crate::spirv::dead::removable_const(self.consts.get(*c)?) {
                return Err(operand(crate::spv::Op::Constant));
            }
        }
        let mut types = types.to_vec();
        types.sort_unstable();
        let declared = self
            .decls
            .iter()
            .filter(|d| matches!(d, Decl::Type(t) if types.binary_search(t).is_ok()))
            .count();
        if types.windows(2).any(|w| w.first() == w.get(1)) || declared != types.len() {
            return Err(operand(crate::spv::Op::TypeVector));
        }
        for t in &types {
            if !matches!(
                self.types.get(*t)?,
                Type::Vector { .. } | Type::Function { .. }
            ) {
                return Err(operand(crate::spv::Op::TypeVector));
            }
        }
        let mut funcs = funcs.to_vec();
        funcs.sort_unstable();
        if funcs.windows(2).any(|w| w.first() == w.get(1))
            || funcs.binary_search(&self.entry.func).is_ok()
            || funcs
                .iter()
                .any(|f| self.functions.get(f.index()).is_none())
        {
            return Err(operand(crate::spv::Op::Function));
        }
        // A copy replaces a removable instruction that stays, by a value of its type.
        let mut copied: Vec<InstAt> = copies.iter().map(|(at, _)| *at).collect();
        copied.sort_unstable();
        if copied.windows(2).any(|w| w.first() == w.get(1))
            || copied.iter().any(|at| insts.binary_search(at).is_ok())
        {
            return Err(operand(crate::spv::Op::CopyObject));
        }
        for (at, to) in copies {
            let node = self
                .blocks
                .get(at.block.index())
                .and_then(|b| b.insts.get(at.index))
                .ok_or(BuildError::Dangling(Dangling::Block(at.block)))?;
            let Some(r) = node.result else {
                return Err(operand(crate::spv::Op::CopyObject));
            };
            let ty = self.value(r)?.ty;
            let facts = self.types.facts(ty)?;
            if !crate::spirv::dead::removable(&node.inst)
                || self.value(*to)?.ty != ty
                || !matches!(
                    facts.shape,
                    Shape::Plain | Shape::Implicit | Shape::Explicit
                )
                || facts.runtime_sized
            {
                return Err(operand(crate::spv::Op::CopyObject));
            }
        }
        let pure = crate::spirv::dead::pure_functions(self);
        for at in &insts {
            let node = self
                .blocks
                .get(at.block.index())
                .and_then(|b| b.insts.get(at.index))
                .ok_or(BuildError::Dangling(Dangling::Block(at.block)))?;
            if node.result.is_none() || !crate::spirv::dead::removable_with(&node.inst, &pure) {
                return Err(operand(crate::spv::Op::Nop));
            }
        }
        // No instruction that stays calls a removed function.
        if !funcs.is_empty() {
            for (bi, b) in self.blocks.iter().enumerate() {
                if funcs.binary_search(&b.func).is_ok() {
                    continue;
                }
                let block = BlockId::new(u32::try_from(bi).unwrap_or(u32::MAX));
                for (index, node) in b.insts.iter().enumerate() {
                    if let Inst::Call { func, .. } = node.inst {
                        if funcs.binary_search(&func).is_ok()
                            && insts.binary_search(&InstAt { block, index }).is_err()
                        {
                            return Err(BuildError::Dangling(Dangling::Func(func)));
                        }
                    }
                }
            }
        }
        for (block, index) in &phis {
            if self
                .blocks
                .get(block.index())
                .and_then(|b| b.phis.get(*index))
                .is_none()
            {
                return Err(BuildError::Dangling(Dangling::Block(*block)));
            }
        }
        for (at, to) in copies {
            self.inst_mut(*at)?.inst = Inst::Unary {
                op: UnOp::CopyObject,
                a: *to,
            };
        }
        for group in insts.chunk_by(|a, b| a.block == b.block) {
            if let Some(b) = group
                .first()
                .and_then(|at| self.blocks.get_mut(at.block.index()))
            {
                remove_at(&mut b.insts, group.iter().map(|at| at.index));
            }
        }
        for group in phis.chunk_by(|a, b| a.0 == b.0) {
            if let Some(b) = group
                .first()
                .and_then(|(block, _)| self.blocks.get_mut(block.index()))
            {
                remove_at(&mut b.phis, group.iter().map(|(_, index)| *index));
            }
        }
        if !consts.is_empty() {
            self.decls
                .retain(|d| !matches!(d, Decl::Const(c) if consts.binary_search(c).is_ok()));
            for c in &consts {
                self.consts.forget(*c);
            }
        }
        if !types.is_empty() {
            self.decls
                .retain(|d| !matches!(d, Decl::Type(t) if types.binary_search(t).is_ok()));
            for t in &types {
                self.types.forget(*t);
            }
        }
        if !funcs.is_empty() {
            self.drop_functions(&funcs);
        }
        self.rederive()
    }

    /// Remove the functions `funcs` (sorted, checked by [`TypedModule::remove_dead`]) with
    /// everything in them, renumbering the functions, blocks, and variables that stay densely in
    /// their order.
    fn drop_functions(&mut self, funcs: &[FuncId]) {
        let gone = |f: FuncId| funcs.binary_search(&f).is_ok();
        let keep_func: Vec<bool> = (0..self.functions.len())
            .map(|i| !gone(FuncId::new(u32::try_from(i).unwrap_or(u32::MAX))))
            .collect();
        let keep_block: Vec<bool> = self.blocks.iter().map(|b| !gone(b.func)).collect();
        let keep_var: Vec<bool> = self.vars.iter().map(|v| !gone(v.func)).collect();
        self.drop_ids(&keep_func, &keep_block, &keep_var);
    }

    /// Remove every function, block, and variable whose entry in `keep_func`, `keep_block`, or
    /// `keep_var` (indexed by id) is false, renumbering the ones that stay densely in their order
    /// (a missing entry keeps its id). A removed function takes its blocks and variables with
    /// it; a block or variable may also leave a function that stays (`spirv::roll`). The values a
    /// removed function, block, or variable defines become [`ValueOrigin::Removed`], and the loops,
    /// switch headers, and `NonUniform` values among them leave their tables. The caller guarantees
    /// that nothing that stays names what leaves; the verifier checks the result. The entry
    /// interface and requirements are not recorded again.
    pub(in crate::spirv) fn drop_ids(
        &mut self,
        keep_func: &[bool],
        keep_block: &[bool],
        keep_var: &[bool],
    ) {
        let dense = |keep: &[bool], len: usize| -> Vec<Option<u32>> {
            let mut next = 0u32;
            (0..len)
                .map(|i| {
                    keep.get(i).copied().unwrap_or(true).then(|| {
                        next += 1;
                        next - 1
                    })
                })
                .collect()
        };
        let fmap = dense(keep_func, self.functions.len());
        let bmap = dense(keep_block, self.blocks.len());
        let vmap = dense(keep_var, self.vars.len());
        let at = |map: &[Option<u32>], i: usize| map.get(i).copied().flatten();
        let func = |f: FuncId| FuncId::new(at(&fmap, f.index()).unwrap_or(u32::MAX));
        let block = |b: BlockId| BlockId::new(at(&bmap, b.index()).unwrap_or(u32::MAX));
        let var = |v: VarId| VarId::new(at(&vmap, v.index()).unwrap_or(u32::MAX));
        // The values the removed functions, blocks, and variables define.
        for d in &mut self.values {
            let kept = match d.origin {
                ValueOrigin::Param { func: f, index } => {
                    at(&fmap, f.index()).map(|f| ValueOrigin::Param {
                        func: FuncId::new(f),
                        index,
                    })
                }
                ValueOrigin::Var(v) => {
                    at(&vmap, v.index()).map(|v| ValueOrigin::Var(VarId::new(v)))
                }
                ValueOrigin::Inst(b) => {
                    at(&bmap, b.index()).map(|b| ValueOrigin::Inst(BlockId::new(b)))
                }
                ValueOrigin::Phi(b) => {
                    at(&bmap, b.index()).map(|b| ValueOrigin::Phi(BlockId::new(b)))
                }
                o @ (ValueOrigin::Const(_) | ValueOrigin::Global(_) | ValueOrigin::Removed) => {
                    Some(o)
                }
            };
            d.origin = kept.unwrap_or(ValueOrigin::Removed);
        }
        let removed = |v: &Value, values: &[ValueDef]| {
            values
                .get(v.index())
                .is_some_and(|d| d.origin == ValueOrigin::Removed)
        };
        let values = &self.values;
        self.non_uniform.retain(|v| !removed(v, values));
        self.loops.retain(|l| at(&bmap, l.header.index()).is_some());
        for l in &mut self.loops {
            l.header = block(l.header);
            l.merge = block(l.merge);
            l.cont = block(l.cont);
            l.preheader = block(l.preheader);
            l.exit_sel = l.exit_sel.map(var);
        }
        self.switches_at_use
            .retain(|b| at(&bmap, b.index()).is_some());
        for b in &mut self.switches_at_use {
            *b = block(*b);
        }
        #[cfg(test)]
        {
            self.uniform_vars = self
                .uniform_vars
                .iter()
                .filter_map(|v| at(&vmap, v.index()).map(VarId::new))
                .collect();
        }
        let mut fi = 0usize;
        self.functions.retain(|_| {
            fi += 1;
            at(&fmap, fi - 1).is_some()
        });
        for f in &mut self.functions {
            f.blocks.retain(|b| at(&bmap, b.index()).is_some());
            for b in &mut f.blocks {
                *b = block(*b);
            }
            f.vars.retain(|v| at(&vmap, v.index()).is_some());
            for v in &mut f.vars {
                *v = var(*v);
            }
        }
        let mut bi = 0usize;
        self.blocks.retain(|_| {
            bi += 1;
            at(&bmap, bi - 1).is_some()
        });
        for b in &mut self.blocks {
            b.func = func(b.func);
            for p in &mut b.phis {
                for (_, from) in &mut p.incoming {
                    *from = block(*from);
                }
            }
            for n in &mut b.insts {
                if let Inst::Call { func: f, .. } = &mut n.inst {
                    *f = func(*f);
                }
            }
            b.merge = match b.merge {
                Some(Merge::Selection { merge }) => Some(Merge::Selection {
                    merge: block(merge),
                }),
                Some(Merge::Loop { merge, cont }) => Some(Merge::Loop {
                    merge: block(merge),
                    cont: block(cont),
                }),
                None => None,
            };
            b.term.for_each_target_mut(|t| *t = block(*t));
        }
        let mut vi = 0usize;
        self.vars.retain(|_| {
            vi += 1;
            at(&vmap, vi - 1).is_some()
        });
        for v in &mut self.vars {
            v.func = func(v.func);
        }
        self.entry.func = func(self.entry.func);
    }

    /// Read specialization constants as data in function `func`: every operand of `func`'s phis,
    /// instructions, and terminators that reads the value of a constant of `local` reads the value
    /// it maps to instead, a value of the constant's type that an instruction of `func` defines
    /// where it dominates every such read (the caller's guarantee; the verifier checks the result).
    /// Then the declarations of `local`'s constants that nothing in the module reads any more leave
    /// it, an operation over others before its operands, so a binder no longer specializes them; a
    /// constant another function or a module-scope part still reads stays, and reads the value the
    /// data carries. Each constant of `local` must be a specialization constant or a
    /// specialization-constant operation, named once. Nothing changes when a check fails. The entry
    /// interface and requirements are recorded again once, as after
    /// [`TypedModule::apply_promotions`].
    pub fn localize_specs(&mut self, func: FuncId, local: &[(ConstId, Value)]) -> BResult<()> {
        if local.is_empty() {
            return Ok(());
        }
        let mut map: std::collections::HashMap<Value, Value> =
            std::collections::HashMap::with_capacity(local.len());
        for &(c, to) in local {
            let def = self.consts.get(c)?;
            if !matches!(def.kind, ConstKind::Spec { .. } | ConstKind::SpecOp { .. }) {
                return Err(operand(crate::spv::Op::SpecConstantOperation));
            }
            let at = self.value(to)?;
            let ValueOrigin::Inst(b) = at.origin else {
                return Err(BuildError::Dangling(Dangling::Value(to)));
            };
            if at.ty != def.ty || self.blocks.get(b.index()).map(|blk| blk.func) != Some(func) {
                return Err(BuildError::Dangling(Dangling::Value(to)));
            }
            if map.insert(self.const_value(c)?, to).is_some() {
                return Err(operand(crate::spv::Op::SpecConstantOperation));
            }
        }
        let swap = |v: &mut Value| {
            if let Some(t) = map.get(v) {
                *v = *t;
            }
        };
        for b in self.blocks.iter_mut().filter(|b| b.func == func) {
            for phi in &mut b.phis {
                for (v, _) in &mut phi.incoming {
                    swap(v);
                }
            }
            for node in &mut b.insts {
                node.inst.for_each_value_mut(swap);
            }
            b.term.for_each_value_mut(swap);
        }
        // What still reads each constant of `local`: an instruction anywhere (an SSA operand or a
        // constant operand), a module-scope part, an initializer, or an array length.
        let mut uses: std::collections::HashMap<Value, usize> =
            map.keys().map(|v| (*v, 0)).collect();
        let mut count = |v: Value| {
            if let Some(u) = uses.get_mut(&v) {
                *u = u.saturating_add(1);
            }
        };
        let const_values = &self.const_values;
        let of = |c: ConstId| const_values.get(c.index()).copied();
        for b in &self.blocks {
            for phi in &b.phis {
                for (v, _) in &phi.incoming {
                    count(*v);
                }
            }
            for node in &b.insts {
                node.inst.for_each_value(&mut count);
                node.inst.for_each_const(|c| {
                    if let Some(v) = of(c) {
                        count(v);
                    }
                });
            }
            b.term.for_each_value(&mut count);
        }
        for d in &self.decls {
            let Decl::Const(c) = d else { continue };
            match &self.consts.get(*c)?.kind {
                ConstKind::Plain(crate::spirv::ir::Constant::Composite { parts, .. }) => {
                    parts.iter().filter_map(|p| of(*p)).for_each(&mut count);
                }
                ConstKind::WorkgroupSize { parts } => {
                    parts.iter().filter_map(|p| of(*p)).for_each(&mut count);
                }
                ConstKind::SpecOp { operands, .. } => {
                    operands.iter().filter_map(|p| of(*p)).for_each(&mut count);
                }
                ConstKind::Plain(_) | ConstKind::Spec { .. } => {}
            }
        }
        let inits = self.globals.iter().filter_map(|g| g.init);
        for c in inits.chain(self.vars.iter().filter_map(|v| v.init)) {
            if let Some(v) = of(c) {
                count(v);
            }
        }
        for (_, t) in self.types.iter() {
            if let Type::Array { len, .. } = t {
                if let Some(v) = of(*len) {
                    count(v);
                }
            }
        }
        // An operation is declared after its operands, so the latest first releases them.
        let mut order: Vec<ConstId> = local.iter().map(|(c, _)| *c).collect();
        order.sort_unstable();
        let mut gone = Vec::new();
        for &c in order.iter().rev() {
            let v = self.const_value(c)?;
            if uses.get(&v).copied().unwrap_or(0) != 0 {
                continue;
            }
            gone.push(c);
            if let ConstKind::SpecOp { operands, .. } = &self.consts.get(c)?.kind {
                for o in operands.iter() {
                    if let Some(u) = of(*o).and_then(|ov| uses.get_mut(&ov)) {
                        *u = u.saturating_sub(1);
                    }
                }
            }
        }
        if !gone.is_empty() {
            gone.sort_unstable();
            self.decls
                .retain(|d| !matches!(d, Decl::Const(c) if gone.binary_search(c).is_ok()));
            for c in &gone {
                self.consts.forget(*c);
            }
        }
        self.rederive()
    }

    /// Record the entry interface and requirements again from the module as it now is, and the
    /// module's own subgroup requirement (`requirements::recorded`), as every edit does once it has
    /// changed the module.
    pub(in crate::spirv) fn rederive(&mut self) -> BResult<()> {
        let view = ModuleView {
            types: &self.types,
            consts: &self.consts,
            values: &self.values,
            globals: &self.globals,
            functions: &self.functions,
            blocks: &self.blocks,
            entry: &self.entry,
            enabling: &self.enabling,
            per_primitive: &self.per_primitive,
        };
        let (iface, reqs) = (interface(&view)?, recorded(&view, &self.subgroup)?);
        self.interface = iface;
        self.requirements = reqs;
        Ok(())
    }

    /// The instruction at `at`, which must be in a block of `func`.
    fn inst_in(&self, func: FuncId, at: InstAt) -> BResult<&InstNode> {
        let block = self
            .blocks
            .get(at.block.index())
            .filter(|b| b.func == func)
            .ok_or(BuildError::Dangling(Dangling::Block(at.block)))?;
        block
            .insts
            .get(at.index)
            .ok_or(BuildError::Dangling(Dangling::Block(at.block)))
    }

    fn inst_mut(&mut self, at: InstAt) -> BResult<&mut InstNode> {
        self.blocks
            .get_mut(at.block.index())
            .and_then(|b| b.insts.get_mut(at.index))
            .ok_or(BuildError::Dangling(Dangling::Block(at.block)))
    }
}

fn remove_at<T>(v: &mut Vec<T>, gone: impl IntoIterator<Item = usize>) {
    let mut gone = gone.into_iter().peekable();
    let mut index = 0usize;
    v.retain(|_| {
        let drop = gone.next_if_eq(&index).is_some();
        while gone.next_if_eq(&index).is_some() {}
        index = index.wrapping_add(1);
        !drop
    });
}

fn operand(op: crate::spv::Op) -> BuildError {
    BuildError::Operand {
        op,
        rule: OperandRule::Category(0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::device::profile::DeviceProfile;
    use crate::pipeline::sym::{ResourceKind, ResourceSlot, Stage};
    use crate::spirv::builder::constructs::ArmEnd;
    use crate::spirv::builder::{FunctionBuilder, ModuleBuilder};
    use crate::spirv::ids::GlobalId;
    use crate::spirv::interp::{run, Buffer, Config, Resources};
    use crate::spirv::ir::{
        natural_layout, BinOp, ExecMode, ExecModel, LayoutRule, MemAccess, StorageClass,
    };
    use crate::spirv::testing::{assert_valid, committed_profiles};

    fn slot(index: u32) -> ResourceSlot {
        ResourceSlot {
            stage: Stage::Kernel,
            kind: ResourceKind::Buffer,
            index,
        }
    }

    fn buffer(b: &mut ModuleBuilder) -> BResult<GlobalId> {
        let u32_ = b.ty(Type::Int {
            width: 32,
            signed: false,
        })?;
        let rta = b.ty(Type::RuntimeArray {
            elem: u32_,
            stride: Some(4),
        })?;
        let layout = natural_layout(b.types(), LayoutRule::Std430, true, &[rta])?;
        let block = b.ty(Type::Struct {
            members: [rta].into(),
            layout: Some(layout),
        })?;
        b.resource(slot(0), StorageClass::StorageBuffer, block)
    }

    fn k(fb: &mut FunctionBuilder<'_>, v: u32) -> BResult<Value> {
        let c = fb.u32_const(v)?;
        fb.module().const_value(c)
    }

    fn word(fb: &mut FunctionBuilder<'_>, buf: GlobalId, i: u32) -> BResult<Value> {
        let base = fb.module().global_value(buf)?;
        let (zero, i) = (k(fb, 0)?, k(fb, i)?);
        fb.access_chain(base, &[zero, i])
    }

    fn moltenvk() -> DeviceProfile {
        committed_profiles()
            .expect("profiles")
            .into_iter()
            .find(|(s, _)| s == "moltenvk-m3")
            .expect("moltenvk-m3")
            .1
    }

    fn run_words(m: &TypedModule, input: &[u32]) -> Vec<u32> {
        let res = Resources {
            buffers: vec![Buffer {
                slot: Some(slot(0)),
                address: None,
                bytes: input.iter().flat_map(|w| w.to_le_bytes()).collect(),
            }],
            push: Vec::new(),
            spec: Default::default(),
        };
        let out = run(m, &Config::default(), res).expect("run");
        out.buffers[0]
            .chunks_exact(4)
            .map(|c| u32::from_le_bytes([c[0], c[1], c[2], c[3]]))
            .collect()
    }

    #[test]
    fn remove_at_drops_sorted_indices_in_order() {
        let mut v = vec![10, 20, 30, 40, 50, 60];
        remove_at(&mut v, []);
        assert_eq!(v, [10, 20, 30, 40, 50, 60]);

        remove_at(&mut v, [0, 2, 2, 5]);
        assert_eq!(v, [20, 40, 50]);

        remove_at(&mut v, [0, 1, 2]);
        assert!(v.is_empty());
    }

    #[test]
    fn apply_promotions_removes_multiple_stores_across_blocks_in_order() {
        let mut b = ModuleBuilder::new();
        let buf = buffer(&mut b).expect("buffer");
        let void = b.ty(Type::Void).expect("void");
        let u32_ = b
            .ty(Type::Int {
                width: 32,
                signed: false,
            })
            .expect("u32");
        let mut fb = b.function(void).expect("func");
        let body = |fb: &mut FunctionBuilder<'_>| -> BResult<()> {
            let p0 = word(fb, buf, 0)?;
            let w0 = fb.load(p0, MemAccess::default())?;
            let v0 = fb.local_var(u32_, None)?;
            let v1 = fb.local_var(u32_, None)?;
            let v2 = fb.local_var(u32_, None)?;
            let vp0 = fb.var_ptr(v0)?;
            let vp1 = fb.var_ptr(v1)?;
            let vp2 = fb.var_ptr(v2)?;
            let c1 = k(fb, 1)?;
            let c2 = k(fb, 2)?;
            let c3 = k(fb, 3)?;
            let a = fb.binary(BinOp::IAdd, w0, c1)?;
            fb.store(vp0, a, MemAccess::default())?;
            let b_val = fb.binary(BinOp::IMul, a, c2)?;
            fb.store(vp1, b_val, MemAccess::default())?;
            let zero = k(fb, 0)?;
            let cond = fb.binary(BinOp::INotEqual, w0, zero)?;
            let picked = fb.if_(
                &mut (),
                cond,
                Box::new(move |fb, _| {
                    let s = fb.binary(BinOp::IAdd, b_val, c3)?;
                    fb.store(vp2, s, MemAccess::default())?;
                    let l2 = fb.load(vp2, MemAccess::default())?;
                    Ok(ArmEnd::FallThrough(vec![l2]))
                }),
                Some(Box::new(move |fb, _| {
                    let l0 = fb.load(vp0, MemAccess::default())?;
                    Ok(ArmEnd::FallThrough(vec![l0]))
                })),
            )?;
            let l0 = fb.load(vp0, MemAccess::default())?;
            let l1 = fb.load(vp1, MemAccess::default())?;
            let sum = fb.binary(BinOp::IAdd, l0, l1)?;
            let total = fb.binary(BinOp::IAdd, sum, picked.0[0])?;
            let p1 = word(fb, buf, 1)?;
            fb.store(p1, total, MemAccess::default())?;
            fb.ret(None)
        };
        body(&mut fb).expect("body");
        let func = fb.finish().expect("finish");
        b.entry(
            ExecModel::GLCompute,
            func,
            vec![ExecMode::LocalSize { x: 1, y: 1, z: 1 }],
        )
        .expect("entry");
        let orig = b.finalize().expect("finalize");
        let mut m = orig.clone();

        let promotions: Vec<Promotion> = m
            .functions()[func.index()]
            .vars
            .iter()
            .map(|&var| {
                let ptr = m.vars[var.index()].value;
                let mut store = None;
                let mut loads = Vec::new();
                for &bid in &m.functions()[func.index()].blocks {
                    for (index, node) in m.blocks()[bid.index()].insts.iter().enumerate() {
                        match node.inst {
                            Inst::Store { ptr: p, .. } if p == ptr => {
                                store = Some(InstAt { block: bid, index });
                            }
                            Inst::Load { ptr: p, .. } if p == ptr => {
                                loads.push(InstAt { block: bid, index });
                            }
                            _ => {}
                        }
                    }
                }
                Promotion {
                    func,
                    var,
                    store: store.expect("store"),
                    loads,
                }
            })
            .collect();
        assert_eq!(promotions.len(), 3);
        let before_insts: usize = m.blocks().iter().map(|blk| blk.insts.len()).sum();
        m.apply_promotions(&promotions).expect("promote");
        let after_insts: usize = m.blocks().iter().map(|blk| blk.insts.len()).sum();
        assert_eq!(after_insts, before_insts - 3);
        assert!(m.functions()[func.index()].vars.is_empty());
        let copies = m
            .blocks()
            .iter()
            .flat_map(|blk| &blk.insts)
            .filter(|n| matches!(n.inst, Inst::Unary { op: UnOp::CopyObject, .. }))
            .count();
        assert_eq!(copies, 4);
        assert_valid(m.clone(), &moltenvk());
        for x in [0u32, 5, 19] {
            assert_eq!(run_words(&m, &[x, 0]), run_words(&orig, &[x, 0]));
        }
    }

    #[test]
    fn remove_dead_drops_multiple_insts_and_phis_per_block_preserving_order() {
        let mut b = ModuleBuilder::new();
        let buf = buffer(&mut b).expect("buffer");
        let void = b.ty(Type::Void).expect("void");
        let mut fb = b.function(void).expect("func");
        let mut dead_values = Vec::new();
        let mut live_phis = Vec::new();
        let mut body = |fb: &mut FunctionBuilder<'_>| -> BResult<()> {
            let p0 = word(fb, buf, 0)?;
            let w0 = fb.load(p0, MemAccess::default())?;
            let c1 = k(fb, 1)?;
            let c2 = k(fb, 2)?;
            let c3 = k(fb, 3)?;
            let c4 = k(fb, 4)?;
            dead_values.push(fb.binary(BinOp::IAdd, w0, c1)?);
            let live_a = fb.binary(BinOp::IAdd, w0, c2)?;
            dead_values.push(fb.binary(BinOp::IMul, w0, c3)?);
            dead_values.push(fb.binary(BinOp::BitwiseXor, w0, c4)?);
            let zero = k(fb, 0)?;
            let cond = fb.binary(BinOp::INotEqual, w0, zero)?;
            let merged = fb.if_(
                &mut (),
                cond,
                Box::new(move |fb, _| {
                    let t0 = fb.binary(BinOp::IAdd, live_a, c1)?;
                    let t1 = fb.binary(BinOp::IAdd, live_a, c2)?;
                    let t2 = fb.binary(BinOp::IAdd, live_a, c3)?;
                    let t3 = fb.binary(BinOp::IAdd, live_a, c4)?;
                    Ok(ArmEnd::FallThrough(vec![t0, t1, t2, t3]))
                }),
                Some(Box::new(move |fb, _| {
                    let e0 = fb.binary(BinOp::IMul, live_a, c1)?;
                    let e1 = fb.binary(BinOp::IMul, live_a, c2)?;
                    let e2 = fb.binary(BinOp::IMul, live_a, c3)?;
                    let e3 = fb.binary(BinOp::IMul, live_a, c4)?;
                    Ok(ArmEnd::FallThrough(vec![e0, e1, e2, e3]))
                })),
            )?;
            live_phis.push(merged.0[1]);
            live_phis.push(merged.0[3]);
            dead_values.push(fb.binary(BinOp::ISub, merged.0[1], c1)?);
            let out = fb.binary(BinOp::IAdd, merged.0[1], merged.0[3])?;
            dead_values.push(fb.binary(BinOp::IMul, out, c2)?);
            let p1 = word(fb, buf, 1)?;
            fb.store(p1, out, MemAccess::default())?;
            fb.ret(None)
        };
        body(&mut fb).expect("body");
        let func = fb.finish().expect("finish");
        b.entry(
            ExecModel::GLCompute,
            func,
            vec![ExecMode::LocalSize { x: 1, y: 1, z: 1 }],
        )
        .expect("entry");
        let orig = b.finalize().expect("finalize");
        let mut m = orig.clone();

        let mut dead_insts = Vec::new();
        let mut merge_block = None;
        for (bi, blk) in m.blocks().iter().enumerate() {
            let block = BlockId::new(bi as u32);
            if blk.phis.len() == 4 {
                merge_block = Some(block);
            }
            for (index, node) in blk.insts.iter().enumerate() {
                if node.result.is_some_and(|r| dead_values.contains(&r)) {
                    dead_insts.push(InstAt { block, index });
                }
            }
        }
        let merge_block = merge_block.expect("merge block");
        assert_eq!(dead_insts.len(), 5);
        let dead_phis = [(merge_block, 0), (merge_block, 2)];
        let before_insts: usize = m.blocks().iter().map(|blk| blk.insts.len()).sum();
        m.remove_dead(&dead_insts, &dead_phis, &[], &[], &[], &[])
            .expect("remove_dead");
        let after_insts: usize = m.blocks().iter().map(|blk| blk.insts.len()).sum();
        assert_eq!(after_insts, before_insts - 5);
        let kept_phis: Vec<Value> = m.blocks()[merge_block.index()]
            .phis
            .iter()
            .map(|p| p.result)
            .collect();
        assert_eq!(kept_phis, live_phis);
        assert_valid(m.clone(), &moltenvk());
        for x in [0u32, 7, 13] {
            assert_eq!(run_words(&m, &[x, 0]), run_words(&orig, &[x, 0]));
        }
    }
}
