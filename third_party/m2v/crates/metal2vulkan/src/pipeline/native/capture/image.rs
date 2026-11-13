use super::{Capture, NONE};
use crate::error::Error;
use crate::spirv::ir::*;
use crate::spv::Op;

fn operands(ops: &ImageOperands) -> [u32; 8] {
    [ops.bias, ops.lod, ops.grad.map(|v| v.0), ops.grad.map(|v| v.1),
     ops.const_offset, ops.offset, ops.sample, ops.min_lod]
        .map(|v| v.map_or(NONE, |v| v.0))
}

impl Capture<'_> {
    pub(super) fn image_instruction(&mut self, node: &InstNode, result: u32, ty: u32) -> Result<bool, Error> {
        let (op, mut args, ops, memory) = match &node.inst {
            Inst::SampledImage { image, sampler } => (Op::SampledImage, vec![image.0, sampler.0], None, None),
            Inst::Image(image) => (Op::Image, vec![image.0], None, None),
            Inst::ImageQuery { query, image } => {
                let mut args = vec![image.0];
                if let ImageQuery::SizeLod(lod) = query { args.push(lod.0); }
                (query.to_spirv(), args, None, None)
            }
            Inst::ImageSample { form, sampled_image, coord, dref, ops }
                if !form.sparse && !form.proj && !form.implicit => {
                (form.to_spirv(), vec![sampled_image.0, coord.0, dref.map_or(NONE, |v| v.0)], Some(ops), None)
            }
            Inst::ImageFetch { image, coord, ops, sparse: false } =>
                (Op::ImageFetch, vec![image.0, coord.0], Some(ops), None),
            Inst::ImageRead { image, coord, ops, sparse: false, mem } =>
                (Op::ImageRead, vec![image.0, coord.0], Some(ops), Some(*mem)),
            Inst::ImageWrite { image, coord, texel, ops, mem } =>
                (Op::ImageWrite, vec![image.0, coord.0, texel.0], Some(ops), Some(*mem)),
            _ => return Ok(false),
        };
        if let Some(ops) = ops { args.extend(operands(ops)); }
        let flags = memory.map_or(0, |m| u32::from(m.volatile())
            | (u32::from(m.non_private()) << 1) | (u32::from(m.made_scope().is_some()) << 2));
        let scope = memory.and_then(TexelMemory::made_scope).map_or(0, |s| s.scope.to_spirv() as u32);
        self.emit(6, op, result, ty, flags, scope, args)?;
        Ok(true)
    }
}
