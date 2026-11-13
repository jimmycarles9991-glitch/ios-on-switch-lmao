// SPDX-License-Identifier: MIT
use crate::direct::native_compute_device;
use crate::direct_compute::{NativeComputePipeline, compiled};
use crate::objects::{NmbComputePipelineDescriptor, NmbDevice, compute_descriptor, e, get, guard};
use crate::reflection::{NmbReflectionNode, Reflection};
use crate::variants::Variants;
use metal2vulkan::Budget;
use metal2vulkan::cache::VariantKey;
use metal2vulkan::pipeline::ComputePipelineDescriptor;
use std::sync::Arc;

#[path = "NativeMetalArtifactCodec.rs"]
mod codec;

pub struct NmbComputeArtifact {
    pub(crate) compile: Arc<metal2vulkan::device::Device>,
    pub(crate) descriptor: ComputePipelineDescriptor,
    pub(crate) base: Arc<NativeComputePipeline>,
    reflection: Option<Reflection>,
    variants: Variants<VariantKey, NativeComputePipeline>,
}

impl NmbComputeArtifact {
    pub(crate) fn pipeline(&self, key: &VariantKey, budget: &Budget) -> Result<Arc<NativeComputePipeline>, String> {
        if *key == VariantKey::new() {
            return Ok(self.base.clone());
        }
        self.variants
            .get(key, || compiled(&self.compile, &self.descriptor, key, budget))
    }
}

#[repr(C)]
pub struct NmbComputeArtifactInfo {
    pub version: u32,
    pub max_threads: u32,
    pub execution_width: u32,
    pub static_memory: u32,
    pub metal_max_threads: u32,
    pub bindings: u32,
}

/// # Safety
/// The descriptor and its function handles are live for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_new(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    reflection: bool,
) -> *mut NmbComputeArtifact {
    guard(std::ptr::null_mut(), || {
        let compile = native_compute_device(get(device)?)?;
        let descriptor = compute_descriptor(descriptor)?;
        let budget = Budget::standard();
        let base = compiled(&compile, &descriptor, &VariantKey::new(), &budget)?;
        let reflection = reflection
            .then(|| {
                base.pipeline
                    .reflection(&descriptor)
                    .map(|r| Reflection::of(&r))
                    .map_err(e("native compute reflection"))
            })
            .transpose()?;
        Ok(Box::into_raw(Box::new(NmbComputeArtifact {
            compile,
            descriptor,
            base: Arc::new(base),
            reflection,
            variants: Variants::default(),
        })))
    })
}

/// # Safety
/// The handle was returned by `nmb_compute_artifact_new` and has no remaining consumers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_release(artifact: *mut NmbComputeArtifact) {
    if !artifact.is_null() {
        // SAFETY: the caller transfers the last ownership of this boxed artifact.
        drop(unsafe { Box::from_raw(artifact) });
    }
}

/// # Safety
/// The artifact is live and `out` names a writable metadata record.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_info(
    artifact: *const NmbComputeArtifact,
    out: *mut NmbComputeArtifactInfo,
) -> bool {
    guard(false, || {
        let artifact = get(artifact)?;
        let info = &artifact.base.metal_info;
        // SAFETY: the caller supplies a metadata record for this call.
        let out = unsafe { out.as_mut() }.ok_or("null compute metadata")?;
        *out = NmbComputeArtifactInfo {
            version: 1,
            max_threads: info.max_total_threads_per_threadgroup,
            execution_width: info.thread_execution_width,
            static_memory: info.static_threadgroup_memory_length,
            metal_max_threads: info.metal_max_total_threads_per_threadgroup,
            bindings: artifact.reflection.as_ref().map_or(0, |r| r.nodes[0].count),
        };
        Ok(true)
    })
}

/// # Safety
/// The artifact is live and output pointers are writable; returned slices live with the artifact.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_reflection(
    artifact: *const NmbComputeArtifact,
    nodes: *mut *const NmbReflectionNode,
    node_count: *mut usize,
    edges: *mut *const u32,
    edge_count: *mut usize,
) -> bool {
    guard(false, || {
        let reflection = get(artifact)?
            .reflection
            .as_ref()
            .ok_or("reflection was not requested")?;
        // SAFETY: the caller's four output pointers name writable fields for this call.
        unsafe {
            *nodes.as_mut().ok_or("null reflection nodes")? = reflection.nodes.as_ptr();
            *node_count.as_mut().ok_or("null reflection node count")? = reflection.nodes.len();
            *edges.as_mut().ok_or("null reflection edges")? = reflection.edges.as_ptr();
            *edge_count.as_mut().ok_or("null reflection edge count")? = reflection.edges.len();
        }
        Ok(true)
    })
}
