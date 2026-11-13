// SPDX-License-Identifier: MIT
use super::*;
use metal2vulkan::cache::{PipelineArtifact, PipelineKey};
use metal2vulkan::pipeline::{ComputePipeline, NativePipeline, NativeProgram, NativeRecord};
use sha2::{Digest, Sha256};
use std::ffi::{CStr, CString, c_void};

const MAGIC: &[u8; 8] = b"SWNART01";
const LIMIT: usize = 64 * 1024 * 1024;

fn key(compile: &metal2vulkan::device::Device, descriptor: &ComputePipelineDescriptor, reflection: bool) -> [u8; 32] {
    let mut h = Sha256::new();
    h.update(MAGIC);
    h.update(metal2vulkan::cache::SOURCE_DIGEST.as_bytes());
    h.update(NativeProgram::VERSION.to_le_bytes());
    h.update(compile.compute_pipeline_key(descriptor).digest());
    h.update([u8::from(reflection)]);
    h.finalize().into()
}

fn word(out: &mut Vec<u8>, value: u32) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn wide(out: &mut Vec<u8>, value: u64) {
    out.extend_from_slice(&value.to_le_bytes());
}
fn section(out: &mut Vec<u8>, bytes: &[u8]) -> Result<(), String> {
    if bytes.len() > LIMIT || out.len().checked_add(bytes.len() + 4).is_none_or(|n| n > LIMIT - 32) {
        return Err("native artifact exceeds limit".into());
    }
    word(out, bytes.len() as u32);
    out.extend_from_slice(bytes);
    Ok(())
}

struct Reader<'a>(&'a [u8]);
impl<'a> Reader<'a> {
    fn take(&mut self, length: usize) -> Result<&'a [u8], String> {
        if length > self.0.len() {
            return Err("truncated native artifact".into());
        }
        let (head, tail) = self.0.split_at(length);
        self.0 = tail;
        Ok(head)
    }
    fn word(&mut self) -> Result<u32, String> {
        Ok(u32::from_le_bytes(self.take(4)?.try_into().unwrap()))
    }
    fn wide(&mut self) -> Result<u64, String> {
        Ok(u64::from_le_bytes(self.take(8)?.try_into().unwrap()))
    }
    fn section(&mut self) -> Result<&'a [u8], String> {
        let n = self.word()? as usize;
        self.take(n)
    }
    fn count(&mut self, minimum: usize) -> Result<usize, String> {
        let n = self.word()? as usize;
        if n > self.0.len() / minimum {
            return Err("invalid native artifact count".into());
        }
        Ok(n)
    }
    fn finish(self) -> Result<(), String> {
        if self.0.is_empty() {
            Ok(())
        } else {
            Err("trailing native artifact data".into())
        }
    }
}

fn encode_program(program: Option<&NativeProgram>) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let Some(p) = program else {
        return Ok(out);
    };
    for value in [
        NativeProgram::VERSION,
        p.types,
        p.values,
        p.blocks,
        p.entry,
        p.records.len() as u32,
    ] {
        word(&mut out, value);
    }
    for r in &p.records {
        for value in [r.kind, r.opcode, r.result, r.ty, r.first, r.count, r.flags, r.auxiliary] {
            word(&mut out, value);
        }
    }
    word(&mut out, p.operands.len() as u32);
    for &value in &p.operands {
        word(&mut out, value);
    }
    if out.len() > LIMIT {
        return Err("native program exceeds limit".into());
    }
    Ok(out)
}

fn decode_program(bytes: &[u8]) -> Result<Option<NativeProgram>, String> {
    if bytes.is_empty() {
        return Ok(None);
    }
    let mut r = Reader(bytes);
    if r.word()? != NativeProgram::VERSION {
        return Err("native program version".into());
    }
    let (types, values, blocks, entry) = (r.word()?, r.word()?, r.word()?, r.word()?);
    let count = r.count(32)?;
    let mut records = Vec::with_capacity(count);
    for _ in 0..count {
        records.push(NativeRecord {
            kind: r.word()?,
            opcode: r.word()?,
            result: r.word()?,
            ty: r.word()?,
            first: r.word()?,
            count: r.word()?,
            flags: r.word()?,
            auxiliary: r.word()?,
        });
    }
    let count = r.count(4)?;
    let mut operands = Vec::with_capacity(count);
    for _ in 0..count {
        operands.push(r.word()?);
    }
    r.finish()?;
    NativeProgram::from_records(records, operands, types, values, blocks, entry)
        .map(Some)
        .ok_or_else(|| "invalid native program".into())
}

fn encode_reflection(reflection: Option<&Reflection>) -> Result<Vec<u8>, String> {
    let mut out = Vec::new();
    let Some(reflection) = reflection else {
        return Ok(out);
    };
    word(&mut out, reflection.nodes.len() as u32);
    for n in &reflection.nodes {
        word(&mut out, n.kind);
        word(&mut out, n.flags);
        for &value in &n.values {
            wide(&mut out, value);
        }
        for &child in &n.children {
            word(&mut out, child);
        }
        word(&mut out, n.first);
        word(&mut out, n.count);
        word(&mut out, u32::from(!n.name.is_null()));
        if !n.name.is_null() {
            // Names point into the immutable artifact's owned CString storage.
            section(&mut out, unsafe { CStr::from_ptr(n.name) }.to_bytes())?;
        }
    }
    word(&mut out, reflection.edges.len() as u32);
    for &edge in &reflection.edges {
        word(&mut out, edge);
    }
    if out.len() > LIMIT {
        return Err("native reflection exceeds limit".into());
    }
    Ok(out)
}

fn decode_reflection(bytes: &[u8]) -> Result<Option<Reflection>, String> {
    if bytes.is_empty() {
        return Ok(None);
    }
    let mut r = Reader(bytes);
    let count = r.count(100)?;
    let mut nodes = Vec::with_capacity(count);
    let mut names = Vec::with_capacity(count);
    for _ in 0..count {
        let kind = r.word()?;
        let flags = r.word()?;
        let mut values = [0; 8];
        for value in &mut values {
            *value = r.wide()?;
        }
        let mut children = [0; 4];
        for child in &mut children {
            *child = r.word()?;
        }
        let first = r.word()?;
        let count = r.word()?;
        let name = match r.word()? {
            0 => None,
            1 => Some(CString::new(r.section()?).map_err(e("native reflection name"))?),
            _ => return Err("invalid native reflection name".into()),
        };
        nodes.push(NmbReflectionNode {
            kind,
            flags,
            name: std::ptr::null(),
            values,
            children,
            first,
            count,
        });
        names.push(name);
    }
    let count = r.count(4)?;
    let mut edges = Vec::with_capacity(count);
    for _ in 0..count {
        edges.push(r.word()?);
    }
    r.finish()?;
    Reflection::from_parts(nodes, edges, names)
        .map(Some)
        .ok_or_else(|| "invalid native reflection graph".into())
}

fn serialize(artifact: &NmbComputeArtifact) -> Result<Vec<u8>, String> {
    let metadata = artifact
        .base
        .pipeline
        .cache_metadata()
        .map_err(e("native artifact module"))?;
    if metadata.key != artifact.compile.compute_pipeline_key(&artifact.descriptor) {
        return Err("native artifact key differs".into());
    }
    let mut out = MAGIC.to_vec();
    out.extend_from_slice(&key(
        &artifact.compile,
        &artifact.descriptor,
        artifact.reflection.is_some(),
    ));
    section(&mut out, &metadata.to_bytes())?;
    section(&mut out, &encode_program(artifact.base.pipeline.program.as_ref())?)?;
    section(&mut out, &encode_reflection(artifact.reflection.as_ref())?)?;
    let checksum = Sha256::digest(&out);
    out.extend_from_slice(&checksum);
    Ok(out)
}

fn deserialize(
    compile: Arc<metal2vulkan::device::Device>,
    descriptor: ComputePipelineDescriptor,
    reflection: bool,
    bytes: &[u8],
) -> Result<NmbComputeArtifact, String> {
    if bytes.len() < 84 || bytes.len() > LIMIT {
        return Err("invalid native artifact size".into());
    }
    let (body, checksum) = bytes.split_at(bytes.len() - 32);
    if Sha256::digest(body).as_slice() != checksum {
        return Err("native artifact checksum".into());
    }
    let mut r = Reader(body);
    if r.take(8)? != MAGIC || r.take(32)? != key(&compile, &descriptor, reflection) {
        return Err("native artifact identity".into());
    }
    let pipeline_key: PipelineKey = compile.compute_pipeline_key(&descriptor);
    let metadata = ComputePipeline::from_bytes_for_key(r.section()?, &pipeline_key, &compile)
        .map_err(e("native artifact metadata"))?;
    let program = decode_program(r.section()?)?;
    let saved_reflection = decode_reflection(r.section()?)?;
    if saved_reflection.is_some() != reflection {
        return Err("native artifact reflection contract".into());
    }
    r.finish()?;
    Ok(NmbComputeArtifact {
        compile,
        descriptor,
        base: Arc::new(NativeComputePipeline {
            pipeline: Arc::new(NativePipeline::cached(metadata, program)),
        }),
        reflection: saved_reflection,
        variants: Variants::default(),
    })
}

/// # Safety
/// The descriptor is live and `out` names 32 writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_key(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    reflection: bool,
    out: *mut u8,
) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null native artifact key".into());
        }
        let compile = native_compute_device(get(device)?)?;
        let descriptor = compute_descriptor(descriptor)?;
        let value = key(&compile, &descriptor, reflection);
        // SAFETY: the caller supplies 32 writable bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(value.as_ptr(), out, value.len());
        }
        Ok(true)
    })
}

/// # Safety
/// The descriptor is live and `bytes` names `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_import(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    reflection: bool,
    bytes: *const c_void,
    length: usize,
) -> *mut NmbComputeArtifact {
    guard(std::ptr::null_mut(), || {
        if bytes.is_null() || length > LIMIT {
            return Err("invalid native artifact bytes".into());
        }
        let compile = native_compute_device(get(device)?)?;
        let descriptor = compute_descriptor(descriptor)?;
        // SAFETY: the caller supplies the counted immutable payload.
        let bytes = unsafe { std::slice::from_raw_parts(bytes.cast::<u8>(), length) };
        Ok(Box::into_raw(Box::new(deserialize(
            compile, descriptor, reflection, bytes,
        )?)))
    })
}

/// # Safety
/// The artifact is live and the callback may read its argument for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_serialize(
    artifact: *const NmbComputeArtifact,
    write: unsafe extern "C" fn(*const c_void, usize, *mut c_void) -> bool,
    context: *mut c_void,
) -> bool {
    guard(false, || {
        let bytes = serialize(get(artifact)?)?;
        // SAFETY: the payload stays live through the caller's synchronous callback.
        Ok(unsafe { write(bytes.as_ptr().cast(), bytes.len(), context) })
    })
}
