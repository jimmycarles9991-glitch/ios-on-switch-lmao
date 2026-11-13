//! A vertex function that chooses its render target slice, linked with a fragment function of the
//! layered storage form and with one that reads `[[render_target_array_index]]`
//! (`pipeline::link::varyings`), on a device of the raw GM20B path's shape:
//!
//! - in a pass of two layers the fetching state runs its layered storage variant, whose fragment
//!   function reads `Layer` to address its storage images, and it links;
//! - a fragment function that reads the slice links under the raw varying, and compiled for a
//!   vertex function that writes none it is still `NeedsInput(RenderTargetArrayIndex)`.
//!
//! `llvm-as` assembles the module; without it the tests skip visibly.

use std::sync::atomic::{AtomicUsize, Ordering};

use crate::bind::BindState;
use crate::device::{Device, DeviceDescription, DevicePolicy};
use crate::error::{Error, RuntimeInput};
use crate::library::Library;
use crate::metal::PixelFormat;
use crate::pipeline::descriptor::ColorAttachmentDescriptor;
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::lower_inputs::LoweringInputs;
use crate::pipeline::testing::{llvm_assemble, AssembleError};
use crate::pipeline::{FunctionDescriptor, RenderPipelineDescriptor, StageSource};
use crate::Budget;

/// `slice_vertex` writes its vertex id as `[[render_target_array_index]]`; `slice_reader` returns
/// the slice it reads, `layered_fetch` adds its position to the color 0 it reads.
const MODULE: &str = r#"target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

define <{ <4 x float>, i32 }> @slice_vertex(i32 noundef %vid) {
entry:
  %f = uitofp i32 %vid to float
  %p = insertelement <4 x float> <float 0.000000e+00, float 0.000000e+00, float 0.000000e+00, float 1.000000e+00>, float %f, i32 0
  %r0 = insertvalue <{ <4 x float>, i32 }> undef, <4 x float> %p, 0
  %r1 = insertvalue <{ <4 x float>, i32 }> %r0, i32 %vid, 1
  ret <{ <4 x float>, i32 }> %r1
}

define <4 x float> @slice_reader(<4 x float> %pos, i32 %layer) {
entry:
  %f = uitofp i32 %layer to float
  %v = insertelement <4 x float> %pos, float %f, i32 2
  ret <4 x float> %v
}

define <4 x float> @layered_fetch(<4 x float> %pos, <4 x float> %previous) {
entry:
  %v = fadd <4 x float> %pos, %previous
  ret <4 x float> %v
}

!air.vertex = !{!0}
!air.fragment = !{!10, !20}
!0 = !{ptr @slice_vertex, !1, !4}
!1 = !{!2, !3}
!2 = !{!"air.position", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!3 = !{!"air.render_target_array_index", !"air.arg_type_name", !"uint", !"air.arg_name", !"layer"}
!4 = !{!5}
!5 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!10 = !{ptr @slice_reader, !11, !13}
!11 = !{!12}
!12 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"color"}
!13 = !{!14, !15}
!14 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"pos"}
!15 = !{i32 1, !"air.render_target_array_index", !"air.arg_type_name", !"uint", !"air.arg_name", !"layer"}
!20 = !{ptr @layered_fetch, !11, !21}
!21 = !{!14, !22}
!22 = !{i32 1, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"previous"}
!air.compile_options = !{!30}
!30 = !{!"air.compile.framebuffer_fetch_enable"}
!air.version = !{!31}
!air.language_version = !{!32}
!31 = !{i32 2, i32 8, i32 0}
!32 = !{!"Metal", i32 4, i32 0, i32 0}
"#;

/// [`MODULE`] assembled by `llvm-as` through a scratch file removed at once, or `None` (skipped
/// visibly) without it
fn module() -> Option<Vec<u8>> {
    static NEXT: AtomicUsize = AtomicUsize::new(0);
    let path = std::env::temp_dir().join(format!(
        "m2v-layered-storage-link-{}-{}.ll",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::write(&path, MODULE).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let assembled = llvm_assemble(&path);
    std::fs::remove_file(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    match assembled {
        Ok(bitcode) => Some(bitcode),
        Err(AssembleError::Missing(why)) => {
            eprintln!("SKIPPED: {why}");
            None
        }
        Err(AssembleError::Rejected(why)) => panic!("llvm-as rejects the module: {why}"),
    }
}

/// A device of the raw GM20B path's shape (no committed profile is vendored): every format the
/// translator asks about supports everything, framebuffer fetch is the pixel interlock, and a
/// vertex function may write `Layer`
fn device() -> Device {
    use crate::device::description::FormatProperties;
    use crate::device::profile::Limits;
    use crate::device::requirements::{ApiVersion, VkExtension, VkFeature};
    use crate::device::FramebufferFetchPolicy;
    use crate::vk;
    let mut desc = DeviceDescription::new(ApiVersion::V1_3);
    desc.limits = Limits {
        max_push_constants_size: 128,
        max_bound_descriptor_sets: 4,
        max_per_stage_descriptor_samplers: 16,
        max_per_stage_descriptor_uniform_buffers: 16,
        max_per_stage_descriptor_storage_buffers: 16,
        max_per_stage_descriptor_sampled_images: 16,
        max_per_stage_descriptor_storage_images: 8,
        max_per_stage_descriptor_input_attachments: 8,
        max_per_stage_resources: 64,
        max_descriptor_set_samplers: 64,
        max_descriptor_set_uniform_buffers: 64,
        max_descriptor_set_storage_buffers: 64,
        max_descriptor_set_sampled_images: 64,
        max_descriptor_set_storage_images: 64,
        max_descriptor_set_input_attachments: 8,
        max_image_dimension_2d: 16384,
        max_image_array_layers: 2048,
        max_vertex_input_attributes: 32,
        max_vertex_input_bindings: 32,
        max_vertex_input_attribute_offset: 2047,
        max_vertex_input_binding_stride: 2048,
        max_vertex_output_components: 128,
        max_fragment_input_components: 128,
        max_fragment_output_attachments: 8,
        max_fragment_combined_output_resources: 16,
        max_color_attachments: 8,
        max_storage_buffer_range: 1 << 27,
        max_uniform_buffer_range: 65536,
        min_storage_buffer_offset_alignment: 16,
        min_uniform_buffer_offset_alignment: 16,
        framebuffer_color_sample_counts: vk::SampleCountFlags::COUNT_1,
        framebuffer_depth_sample_counts: vk::SampleCountFlags::COUNT_1,
        ..Limits::default()
    };
    for &f in crate::device::description::FormatSupport::queries() {
        let all = vk::FormatFeatureFlags2::from_raw(u64::MAX);
        let mut props = FormatProperties::default();
        props.optimal_tiling = all;
        props.linear_tiling = all;
        props.buffer = all;
        props.color_attachment_sample_counts = vk::SampleCountFlags::COUNT_1;
        desc.formats.formats.insert(f, props);
    }
    desc.features.extend([
        VkFeature::FragmentStoresAndAtomics,
        VkFeature::ShaderStorageImageExtendedFormats,
        VkFeature::IndependentBlend,
        VkFeature::ShaderOutputLayer,
        VkFeature::FragmentShaderPixelInterlock,
    ]);
    desc.extensions
        .insert(VkExtension::ExtFragmentShaderInterlock);
    let mut policy = DevicePolicy::default();
    policy.framebuffer_fetch = FramebufferFetchPolicy::RequireOrdered;
    Device::new(desc, policy).expect("test device")
}

/// A render pipeline of `slice_vertex` and `fragment`
fn render(lib: &Library, fragment: StageSource) -> RenderPipelineDescriptor {
    let vertex = FunctionDescriptor::new(lib.function("slice_vertex").expect("slice_vertex"));
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex));
    d.fragment = Some(fragment);
    d
}

fn function(lib: &Library, name: &str) -> StageSource {
    StageSource::Function(FunctionDescriptor::new(
        lib.function(name).unwrap_or_else(|e| panic!("{name}: {e}")),
    ))
}

#[test]
fn a_layered_storage_fragment_links_with_a_vertex_function_that_chooses_its_slice() {
    let Some(bc) = module() else {
        return;
    };
    let lib = Library::from_air(&bc).expect("library");
    let dev = device();
    let budget = Budget::standard();
    let mut d = render(&lib, function(&lib, "layered_fetch"));
    d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm));
    let p = dev.compile_render_pipeline(&d, &budget).expect("pipeline");
    let layered = BindState {
        render_target_array_length: 2,
        ..Default::default()
    };
    let plan = p.plan(&dev, &layered.shape()).expect("layered plan");
    assert!(plan.variant.layered_storage, "{:?}", plan.variant);
    assert!(!plan.variant.dropped_vertex_layer, "{:?}", plan.variant);
    let variant = dev
        .compile_render_variant(&d, &plan.variant, &budget)
        .expect("layered variant");
    let facts: Vec<RuntimeFact> = variant.dependencies.iter().map(|d| d.fact).collect();
    assert!(facts.contains(&RuntimeFact::LayeredStorage), "{facts:?}");
    assert!(
        facts.contains(&RuntimeFact::RenderTargetArrayLength {
            layer_dropped: false
        }),
        "{facts:?}"
    );
}

#[test]
fn a_fragment_that_reads_the_slice_compiled_for_another_vertex_function_is_refused() {
    let Some(bc) = module() else {
        return;
    };
    let lib = Library::from_air(&bc).expect("library");
    let dev = device();
    let budget = Budget::standard();
    dev.compile_render_pipeline(&render(&lib, function(&lib, "slice_reader")), &budget)
        .expect("the reader under the raw varying");
    let reader = FunctionDescriptor::new(lib.function("slice_reader").expect("slice_reader"));
    let without = dev
        .compile_function(&reader, &LoweringInputs::new(), &budget)
        .expect("fragment");
    assert_eq!(
        dev.compile_render_pipeline(&render(&lib, StageSource::Compiled(without)), &budget)
            .err(),
        Some(Error::NeedsInput(RuntimeInput::RenderTargetArrayIndex))
    );
}
