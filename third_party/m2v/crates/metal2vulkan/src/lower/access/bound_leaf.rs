use super::emit::Mem;
use crate::memory::leaf::{self, k32, known, Leaf, Path, WordMem};
use crate::spirv::builder::constructs::{ArmEnd, ArmFn};
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::{BResult, BuildError, Dangling};
use crate::spirv::ids::Value;
use crate::spirv::ir::BinOp;

fn needs_alignment(mem: &Mem, leaf: &Leaf) -> bool {
    matches!(mem, Mem::View(_)) && leaf.path == Path::Vec4 && mem.units().vec4
}

fn aligned(fb: &mut FunctionBuilder<'_>, offset: Value) -> BResult<Value> {
    let mask = k32(fb, 15)?;
    let low = fb.binary(BinOp::BitwiseAnd, offset, mask)?;
    let zero = k32(fb, 0)?;
    fb.binary(BinOp::IEqual, low, zero)
}

pub(super) fn load(
    fb: &mut FunctionBuilder<'_>,
    mut mem: Mem,
    leaf: Leaf,
    offset: Value,
) -> BResult<Value> {
    if !needs_alignment(&mem, &leaf) {
        return leaf::load(fb, &mut mem, &leaf, offset);
    }
    let words = Leaf {
        path: Path::Aligned,
        ..leaf
    };
    if let Some(offset_bits) = known(fb, offset)? {
        let selected = if offset_bits & 15 == 0 { leaf } else { words };
        return leaf::load(fb, &mut mem, &selected, offset);
    }
    // AIR aligns the address within a Metal binding. A nonzero binding residual can lower its absolute alignment.
    let condition = aligned(fb, offset)?;
    let vector: ArmFn<'_, Mem> = Box::new(move |fb, mem| {
        Ok(ArmEnd::FallThrough(vec![leaf::load(
            fb, mem, &leaf, offset,
        )?]))
    });
    let word: ArmFn<'_, Mem> = Box::new(move |fb, mem| {
        Ok(ArmEnd::FallThrough(vec![leaf::load(
            fb, mem, &words, offset,
        )?]))
    });
    fb.if_(&mut mem, condition, vector, Some(word))?
        .0
        .first()
        .copied()
        .ok_or(BuildError::Dangling(Dangling::Value(Value::new(u32::MAX))))
}

pub(super) fn store(
    fb: &mut FunctionBuilder<'_>,
    mut mem: Mem,
    leaf: Leaf,
    offset: Value,
    value: Value,
) -> BResult<bool> {
    if !needs_alignment(&mem, &leaf) {
        return leaf::store(fb, &mut mem, &leaf, offset, value);
    }
    let words = Leaf {
        path: Path::Aligned,
        ..leaf
    };
    if let Some(offset_bits) = known(fb, offset)? {
        let selected = if offset_bits & 15 == 0 { leaf } else { words };
        return leaf::store(fb, &mut mem, &selected, offset, value);
    }
    let condition = aligned(fb, offset)?;
    let vector: ArmFn<'_, Mem> = Box::new(move |fb, mem| {
        leaf::store(fb, mem, &leaf, offset, value)?;
        Ok(ArmEnd::FallThrough(Vec::new()))
    });
    let word: ArmFn<'_, Mem> = Box::new(move |fb, mem| {
        leaf::store(fb, mem, &words, offset, value)?;
        Ok(ArmEnd::FallThrough(Vec::new()))
    });
    fb.if_(&mut mem, condition, vector, Some(word))?;
    Ok(false)
}
