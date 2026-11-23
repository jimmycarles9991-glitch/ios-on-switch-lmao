//! A `Depth16Unorm` attachment whose draws run their depth test in the fragment function: the
//! stand-in holds the attachment's codes as floats, and the variant's function tests and writes the
//! code Metal stores for its depth. The public fixture `fragment_depth_stencil` returns a depth and a
//! stencil; beside a stencil attachment, on a device with the pixel interlock and no stencil
//! export, a draw whose stencil write follows its depth test runs both tests in the function. No
//! GPU is used.
//!
//! `llvm-as` assembles the fixtures; without it the tests fail.

use crate::bind::{BindState, FragmentTests, WriteSource};
use crate::budget::Budget;
use crate::device::{Device, DeviceDescription, DevicePolicy};
use crate::error::{Error, RuntimeInput};
use crate::library::Library;
use crate::metal::state::DepthStencilDescriptor;
use crate::metal::{CompareFunction, PixelFormat, StencilOperation};
use crate::pipeline::descriptor::{FunctionDescriptor, RenderPipelineDescriptor, StageSource};
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::testing::{engine_dir, fixtures_dir, llvm_assemble};
use crate::spirv::testing::check_words_valid;

fn assemble(path: &std::path::Path) -> Vec<u8> {
    llvm_assemble(path).unwrap_or_else(|e| panic!("llvm-as on {}: {e:?}", path.display()))
}

fn device() -> Device {
    let path = fixtures_dir().join("../../profiles/nvidia-rtx50-linux.json");
    let text = std::fs::read_to_string(path).expect("profile");
    let desc = DeviceDescription::from_khronos_json(&text, None).expect("description");
    Device::new(desc, DevicePolicy::default()).expect("device")
}

/// The fixtures' pipeline over a `depth` and a `stencil` attachment.
fn render(depth: PixelFormat, stencil: PixelFormat) -> RenderPipelineDescriptor {
    let io = assemble(&engine_dir().join("stage_io.ll"));
    let tested = assemble(&fixtures_dir().join("fragment_depth_stencil.ll"));
    let function = |bc: &[u8], name: &str| {
        StageSource::Function(FunctionDescriptor::new(
            Library::from_air(bc)
                .expect("library")
                .function(name)
                .expect("function"),
        ))
    };
    let mut d = RenderPipelineDescriptor::new(function(&io, "stage_io_vertex_position"));
    d.fragment = Some(function(&tested, "fragment_depth_stencil"));
    d.depth_format = Some(depth);
    d.stencil_format = Some(stencil);
    d
}

/// A draw that tests depth `Less` and whose stencil write follows its depth test.
fn state() -> BindState {
    let mut ds = DepthStencilDescriptor::default();
    ds.depth_compare_function = CompareFunction::Less;
    ds.front_face_stencil.depth_failure_operation = StencilOperation::IncrementClamp;
    ds.front_face_stencil.depth_stencil_pass_operation = StencilOperation::Replace;
    BindState {
        depth_stencil: ds,
        fragment_tests: FragmentTests::Stencil,
        ..Default::default()
    }
}

/// Beside a `Stencil8`, a `Depth16Unorm` draw runs the variant whose function tests in the shader
/// against the attachment's codes, as a `Depth32Float_Stencil8` one runs the variant that tests
/// floats: each variant states the in-shader tests, passes L3, binds the depth stand-in and plans
/// itself; the two variants differ, and neither links into the other's pipeline, whose stand-in
/// holds the other form.
#[test]
fn a_depth16_attachment_tests_its_codes_in_the_shader() {
    let dev = device();
    let budget = Budget::standard();
    let codes = render(PixelFormat::Depth16Unorm, PixelFormat::Stencil8);
    let floats = render(
        PixelFormat::Depth32FloatStencil8,
        PixelFormat::Depth32FloatStencil8,
    );
    let mut variants = Vec::new();
    for (d, form) in [(&codes, "Depth16Unorm"), (&floats, "Depth32Float_Stencil8")] {
        let p = dev.compile_render_pipeline(d, &budget).expect("pipeline");
        let plan = p
            .plan(&dev, &state().shape())
            .unwrap_or_else(|e| panic!("{form}: {e:?}"));
        assert!(
            plan.variant.shader_tests && plan.variant.shader_stencil,
            "{form}: {:?}",
            plan.variant
        );
        let v = dev
            .compile_render_variant(d, &plan.variant, &budget)
            .unwrap_or_else(|e| panic!("{form}: {e}"));
        assert!(v
            .dependencies
            .iter()
            .any(|x| matches!(x.fact, RuntimeFact::ShaderTests { .. })));
        for m in &v.stages {
            check_words_valid(&m.spirv, &v.requirements, dev.profile())
                .unwrap_or_else(|e| panic!("{form}: L3: {e}"));
        }
        let own = v.plan(&dev, &state().shape()).expect("the variant's plan");
        assert_eq!(own.variant, plan.variant, "the variant names itself");
        assert!(own
            .descriptors
            .iter()
            .any(|w| w.source == WriteSource::DepthStandIn));
        variants.push(plan.variant);
    }
    assert_ne!(variants[0], variants[1]);
    for (d, other) in [(&codes, &variants[1]), (&floats, &variants[0])] {
        assert_eq!(
            dev.compile_render_variant(d, other, &budget).map(|_| ()),
            Err(Error::NeedsInput(RuntimeInput::FramebufferFetch))
        );
    }
}
