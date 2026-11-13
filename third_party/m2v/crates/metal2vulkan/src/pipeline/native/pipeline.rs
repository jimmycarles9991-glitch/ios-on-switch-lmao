use super::{NativeFunction, NativeProgram};
use crate::budget::{Budget, Meter, SPIRV_WORDS};
use crate::cache::PipelineKey;
use crate::device::{Device, DeviceProfile};
use crate::error::{Error, Feature, Unsupported};
use crate::library::FunctionArtifact;
use crate::pipeline::artifact::ComputePipeline;
use crate::pipeline::compile::{api, guarded};
use crate::pipeline::descriptor::ComputePipelineDescriptor;
use crate::pipeline::link::LinkTable;
use crate::spirv::ir::TypedModule;
use std::sync::{Arc, OnceLock};

/// Native stage metadata and its retained source. `metadata.stage.spirv` is empty;
/// only `portable_words` materializes a portable representation.
#[derive(Debug)]
pub struct NativePipeline {
    pub metadata: ComputePipeline,
    pub program: Option<NativeProgram>,
    source: Source,
    portable: OnceLock<Result<Arc<[u32]>, Error>>,
}

#[derive(Debug)]
enum Source {
    Typed {
        artifact: Arc<FunctionArtifact<TypedModule>>,
        profile: DeviceProfile,
        link: LinkTable,
    },
    Cached(Arc<[u32]>),
}

impl NativeFunction {
    pub fn into_pipeline(
        self,
        device: &Device,
        d: &ComputePipelineDescriptor,
    ) -> Result<NativePipeline, Error> {
        guarded(|| {
            if let Some(input) = &d.stage_input {
                input.check_layouts()?;
            }
            let a = &self.artifact;
            api::check_stage_in(a, d)?;
            api::check_call_stack(a, d.max_call_stack_depth)?;
            let linked = crate::pipeline::link::link_compute(&a.interface, device.profile())?;
            let stage = api::stage_metadata(a, &linked, 0, Arc::from([]))?;
            let profile = device.profile_fingerprint();
            let key: PipelineKey = crate::pipeline::compile::key::pipeline_key(
                b"compute",
                &profile,
                &[a.key],
                &api::compute_fields(d),
            );
            let metadata = ComputePipeline {
                key,
                profile,
                stage,
                layout: linked.layout,
                dependencies: api::dependencies(&[a.as_ref()], device),
                interface: linked.interface,
                metal_info: device.metal_info(
                    a,
                    d.max_total_threads_per_threadgroup,
                    d.support_indirect_command_buffers,
                ),
                requirements: a.requirements.clone(),
                variant_bits: a.variant_bits,
            };
            Ok(NativePipeline {
                metadata,
                program: self.program,
                source: Source::Typed {
                    artifact: self.artifact,
                    profile: device.profile().clone(),
                    link: linked.table,
                },
                portable: OnceLock::new(),
            })
        })
    }
}

impl NativePipeline {
    pub fn cached(mut metadata: ComputePipeline, program: Option<NativeProgram>) -> Self {
        let words = std::mem::replace(&mut metadata.stage.spirv, Arc::from([]));
        Self {
            metadata,
            program,
            source: Source::Cached(words),
            portable: OnceLock::new(),
        }
    }

    pub fn cache_metadata(&self) -> Result<ComputePipeline, Error> {
        let mut metadata = self.metadata.clone();
        metadata.stage.spirv = Arc::from(self.portable_words()?);
        Ok(metadata)
    }

    pub fn portable_words(&self) -> Result<&[u32], Error> {
        let (artifact, profile, link) = match &self.source {
            Source::Typed { artifact, profile, link } => (artifact, profile, link),
            Source::Cached(words) => return Ok(words),
        };
        self.portable
            .get_or_init(|| {
                guarded(|| {
                    let mut meter = Meter::new(&Budget::standard());
                    let module = &artifact.module;
                    meter.charge(crate::spirv::serialize::id_table_bytes(module, profile))?;
                    let words = crate::spirv::serialize::serialize_ref_for(module, profile)?;
                    meter.charge(words.footprint())?;
                    crate::lower::estimate::check_emitted(words.words.len(), SPIRV_WORDS)?;
                    Ok(crate::spirv::serialize::relocate(&words, link)?.into())
                })
            })
            .as_ref()
            .map(|v| v.as_ref())
            .map_err(Clone::clone)
    }

    pub fn portable_is_materialized(&self) -> bool {
        matches!(self.source, Source::Cached(_)) || self.portable.get().is_some_and(Result::is_ok)
    }

    pub fn reflection(
        &self,
        d: &ComputePipelineDescriptor,
    ) -> Result<crate::reflection::ComputePipelineReflection, Error> {
        let stage = match &self.source {
            Source::Typed { artifact, .. } => artifact.reflection.clone(),
            Source::Cached(_) => None,
        }
        .ok_or_else(|| Error::Unsupported(Unsupported::not_implemented(Feature::Reflection)))?;
        d.reflection_from(stage)
    }
}
