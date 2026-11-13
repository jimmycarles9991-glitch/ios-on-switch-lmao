use crate::budget::{Meter, vec_bytes};
use crate::error::Error;
use crate::library::FunctionArtifact;
use crate::pipeline::link::LinkTable;
use crate::pipeline::sym::Symbol;
use crate::spirv::serialize::SymbolTable;

mod capture;
mod pipeline;
pub use pipeline::NativePipeline;

/// A verified typed lowering with the same metadata as a portable function artifact.
#[derive(Clone, Debug)]
pub struct NativeFunction {
    pub(crate) artifact: std::sync::Arc<FunctionArtifact<crate::spirv::ir::TypedModule>>,
    pub program: Option<NativeProgram>,
}

/// One typed declaration, SSA operation, or control-flow edge. Operand identifiers refer to
/// the source type/value/block arenas, not to serialized module identifiers. `opcode` uses
/// the corresponding SPIR-V enumerant only as a stable operation vocabulary.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NativeRecord {
    pub kind: u32,
    pub opcode: u32,
    pub result: u32,
    pub ty: u32,
    pub first: u32,
    pub count: u32,
    pub flags: u32,
    pub auxiliary: u32,
}

/// Immutable arrays consumed by the native NIR builder. A missing program means the whole
/// stage contains an operation or contract that this version does not yet represent.
#[derive(Clone, Debug)]
pub struct NativeProgram {
    pub records: Vec<NativeRecord>,
    pub operands: Vec<u32>,
    pub types: u32,
    pub values: u32,
    pub blocks: u32,
    pub entry: u32,
    links: Vec<(usize, Symbol)>,
}

impl NativeProgram {
    pub const VERSION: u32 = 1;

    pub fn from_records(
        records: Vec<NativeRecord>,
        operands: Vec<u32>,
        types: u32,
        values: u32,
        blocks: u32,
        entry: u32,
    ) -> Option<Self> {
        if types == 0
            || types > 1 << 20
            || values == 0
            || values > 1 << 24
            || blocks == 0
            || blocks > 1 << 20
            || entry >= blocks
            || records.len() > 1 << 24
            || operands.len() > 1 << 26
            || types as usize > records.len()
            || values as usize > records.len().saturating_add(operands.len())
            || blocks as usize > records.len()
            || records.iter().any(|r| {
                r.kind > 8
                    || r.first as usize > operands.len()
                    || r.count as usize > operands.len() - r.first as usize
            })
        {
            return None;
        }
        Some(Self {
            records,
            operands,
            types,
            values,
            blocks,
            entry,
            links: Vec::new(),
        })
    }

    pub(crate) fn link(&mut self, table: &LinkTable) -> Result<(), Error> {
        for (at, symbol) in &self.links {
            self.operands[*at] = table.resolve(symbol).ok_or_else(|| {
                crate::spirv::error::BuildError::Reloc(
                    crate::spirv::error::RelocRule::MissingSymbol(symbol.clone()),
                )
            })?;
        }
        self.links.clear();
        Ok(())
    }

    fn footprint(&self) -> usize {
        vec_bytes(&self.records) + vec_bytes(&self.operands) + vec_bytes(&self.links)
    }

    fn record(
        &mut self,
        m: &mut Meter,
        mut record: NativeRecord,
        operands: impl IntoIterator<Item = u32>,
    ) -> Result<usize, Error> {
        record.first = self.operands.len() as u32;
        for word in operands {
            m.grow(&mut self.operands, 1)?;
            self.operands.push(word);
        }
        record.count = self.operands.len() as u32 - record.first;
        m.grow(&mut self.records, 1)?;
        let at = self.records.len();
        self.records.push(record);
        Ok(at)
    }

    fn symbol(&mut self, m: &mut Meter, at: usize, symbol: Symbol) -> Result<(), Error> {
        m.grow(&mut self.links, 1)?;
        self.links.push((at, symbol));
        Ok(())
    }
}

pub(crate) use capture::capture;
