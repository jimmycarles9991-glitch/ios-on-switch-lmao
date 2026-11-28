//! Vertex amplification beside the layered storage form, on [`engine_layered_storage_link`]'s
//! module and device of the raw GM20B path's shape: an amplified draw drawn one amplification at a
//! time takes the storage form, as a fetching function and as a color its pass keeps out of its
//! subpass; drawn as multiview views it is refused.
//!
//! `llvm-as` assembles the module; without it the tests skip visibly.

use super::engine_layered_storage_link::{device, function, module, stand_ins, R32F};
use crate::bind::{BindState, FragmentTests, VertexAmplification, ViewMapping};
use crate::error::{BindError, Feature, Needs};
use crate::library::Library;
use crate::metal::PixelFormat;
use crate::pipeline::descriptor::ColorAttachmentDescriptor;
use crate::pipeline::facts::RuntimeFact;
use crate::pipeline::{FunctionDescriptor, RenderPipelineDescriptor, StageSource};
use crate::Budget;

/// A fetching state of vertex amplification (`maxVertexAmplificationCount` 2) whose fragment
/// function discards, beside a `Depth32Float` attachment, as Weather's VFX particles are: drawn one
/// amplification at a time into a pass of two layers, a draw whose depth test writes runs the
/// layered in-shader depth test variant, each amplification's fragments at its own layer, as the
/// raw path draws every amplified state. Drawn as multiview views it is refused: one image of the
/// pass's layers addressed at `Layer` does not model what each view reads and writes.
#[test]
fn an_amplified_fetching_state_takes_the_storage_form_drawn_one_amplification_at_a_time() {
    let Some(bc) = module() else {
        return;
    };
    let lib = Library::from_air(&bc).expect("library");
    let dev = device();
    let budget = Budget::standard();
    let vertex = FunctionDescriptor::new(lib.function("plain_vertex").expect("plain_vertex"));
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex));
    d.fragment = Some(function(&lib, "layered_fetch_discard"));
    d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm));
    d.depth_format = Some(PixelFormat::Depth32Float);
    d.max_vertex_amplification_count = 2;
    let p = dev.compile_render_pipeline(&d, &budget).expect("pipeline");
    let shape = |one_at_a_time: bool| {
        let mut amplification =
            VertexAmplification::new(2, vec![ViewMapping::new(0, 0), ViewMapping::new(0, 1)]);
        amplification.one_at_a_time = one_at_a_time;
        BindState {
            fragment_tests: FragmentTests::Depth { writes: true },
            render_target_array_length: 2,
            vertex_amplification: amplification,
            ..Default::default()
        }
        .shape()
    };
    let drawn = p.plan(&dev, &shape(true)).expect("one amplification at a time");
    assert!(
        drawn.variant.shader_tests
            && drawn.variant.layered_storage
            && drawn.variant.amplification_draws.is_some(),
        "{:?}",
        drawn.variant
    );
    let variant = dev
        .compile_render_variant(&d, &drawn.variant, &budget)
        .expect("the amplified in-shader depth test");
    let facts: Vec<RuntimeFact> = variant.dependencies.iter().map(|d| d.fact).collect();
    assert!(facts.contains(&RuntimeFact::LayeredStorage), "{facts:?}");
    assert_eq!(stand_ins(&variant.stages, R32F), vec![true], "one 2D-array stand-in");
    let again = variant.plan(&dev, &shape(true)).expect("the variant's plan");
    assert_eq!(again.variant, drawn.variant, "a fixed point");
    match p.plan(&dev, &shape(false)) {
        Err(BindError::Unsupported(u)) => assert_eq!(
            (u.feature, u.needs),
            (Feature::FramebufferFetch, Needs::NotImplemented)
        ),
        other => panic!("as views: {:?}", other.map(|_| ())),
    }
}

/// A color a draw's pass keeps out of its subpass for other draws' storage form, which the
/// fragment function writes but does not read (`BindState::interlocked_colors`), under vertex
/// amplification: drawn one amplification at a time into a pass of two layers, the variant writes
/// it through the layered storage form; drawn as multiview views it is refused.
#[test]
fn an_amplified_draw_writes_an_interlocked_color_one_amplification_at_a_time() {
    let Some(bc) = module() else {
        return;
    };
    let lib = Library::from_air(&bc).expect("library");
    let dev = device();
    let budget = Budget::standard();
    let vertex = FunctionDescriptor::new(lib.function("plain_vertex").expect("plain_vertex"));
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(vertex));
    d.fragment = Some(function(&lib, "plain_color"));
    d.color_attachments[0] = Some(ColorAttachmentDescriptor::new(PixelFormat::RGBA8Unorm));
    d.max_vertex_amplification_count = 2;
    let p = dev.compile_render_pipeline(&d, &budget).expect("pipeline");
    assert!(p.graphics.storage_attachments.is_empty());
    let shape = |one_at_a_time: bool| {
        let mut amplification =
            VertexAmplification::new(2, vec![ViewMapping::new(0, 0), ViewMapping::new(0, 1)]);
        amplification.one_at_a_time = one_at_a_time;
        BindState {
            interlocked_colors: [0].into(),
            render_target_array_length: 2,
            vertex_amplification: amplification,
            ..Default::default()
        }
        .shape()
    };
    let drawn = p.plan(&dev, &shape(true)).expect("one amplification at a time");
    assert_eq!(drawn.variant.interlocked_colors, [0].into());
    assert!(
        drawn.variant.layered_storage && drawn.variant.amplification_draws.is_some(),
        "{:?}",
        drawn.variant
    );
    let variant = dev
        .compile_render_variant(&d, &drawn.variant, &budget)
        .expect("the amplified interlocked color's variant");
    let stored: Vec<(u32, bool)> = variant
        .graphics
        .storage_attachments
        .iter()
        .map(|a| (a.color_index, a.writes))
        .collect();
    assert_eq!(stored, [(0, true)]);
    let facts: Vec<RuntimeFact> = variant.dependencies.iter().map(|d| d.fact).collect();
    assert!(facts.contains(&RuntimeFact::LayeredStorage), "{facts:?}");
    let again = variant.plan(&dev, &shape(true)).expect("the variant's plan");
    assert_eq!(again.variant, drawn.variant, "a fixed point");
    match p.plan(&dev, &shape(false)) {
        Err(BindError::Unsupported(u)) => assert_eq!(
            (u.feature, u.needs),
            (Feature::FramebufferFetch, Needs::NotImplemented)
        ),
        other => panic!("as views: {:?}", other.map(|_| ())),
    }
}
