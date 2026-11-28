use std::collections::BTreeMap;

use super::*;
use crate::cache::VariantKey;
use crate::metal::{BlendFactor, PixelFormat};
use crate::pipeline::lower_inputs::ChannelWrite;

fn pipeline(lib: &Library, colors: &[(PixelFormat, bool)]) -> RenderPipelineDescriptor {
    let mut d = RenderPipelineDescriptor::new(StageSource::Function(FunctionDescriptor::new(
        lib.function("stage_io_vertex_position").expect("vertex"),
    )));
    d.fragment = Some(StageSource::Function(FunctionDescriptor::new(
        lib.function("stage_io_fragment_primitive").expect("fragment"),
    )));
    for (i, &(format, blended)) in colors.iter().enumerate() {
        let mut c = ColorAttachmentDescriptor::new(format);
        if blended {
            c.blending_enabled = true;
            c.source_rgb_blend_factor = BlendFactor::SourceAlpha;
            c.destination_rgb_blend_factor = BlendFactor::OneMinusSourceAlpha;
        }
        d.color_attachments[i] = Some(c);
    }
    d
}

#[test]
fn a_fragment_key_reads_only_the_attachments_and_channel_rules_its_lowering_reads() {
    let Some(bc) = fixture("stage_io") else {
        return;
    };
    let dev = device("lunarg-desktop-baseline-2023");
    let lib = Library::from_air(&bc).expect("library");
    let fragment =
        FunctionDescriptor::new(lib.function("stage_io_fragment_primitive").expect("fragment"));
    let keys = |colors: &[(PixelFormat, bool)]| {
        dev.render_pipeline_key(&pipeline(&lib, colors), &Budget::standard())
            .expect("keys")
    };
    let compiled = |colors: &[(PixelFormat, bool)]| {
        let inputs = keys(colors).fragment_inputs.expect("fragment inputs");
        dev.compile_function(&fragment, &inputs, &Budget::standard())
            .expect("fragment")
    };
    let base = [(PixelFormat::BGRA8Unorm, true)];
    let narrow = [(PixelFormat::R8Unorm, true)];
    let second = [
        (PixelFormat::BGRA8Unorm, true),
        (PixelFormat::BGRA8Unorm, false),
    ];

    let inputs = keys(&second).fragment_inputs.expect("fragment inputs");
    assert_eq!(
        inputs.color_writes.into_iter().collect::<Vec<_>>(),
        [(0, [Some(ChannelWrite::Device), None, None, None])],
        "the inputs the lowering reads: color 0's, in its read form"
    );
    let want = compiled(&base);
    for colors in [&narrow[..], &second[..]] {
        assert_eq!(keys(colors).fragment, keys(&base).fragment, "{colors:?}");
        assert_eq!(compiled(colors).artifact(), want.artifact(), "{colors:?}");
        let mut d = pipeline(&lib, colors);
        let cold = dev.compile_render_pipeline(&d, &Budget::standard());
        d.fragment = Some(StageSource::Compiled(want.clone()));
        let taken = dev.compile_render_pipeline(&d, &Budget::standard());
        match (cold, taken) {
            (Ok(cold), Ok(taken)) => assert_eq!(taken, cold, "{colors:?}"),
            (Err(Error::Unsupported(_)), Err(Error::Unsupported(_))) => {}
            (cold, taken) => panic!("{colors:?}: {cold:?} against {taken:?}"),
        }
    }
    assert_ne!(
        keys(&[(PixelFormat::BGRA8UnormSRGB, false)]).fragment,
        keys(&[(PixelFormat::BGRA8Unorm, false)]).fragment,
        "an sRGB encode is read"
    );

    // Direct compile_function preserves its function_compile_key contract even with extra or
    // unnormalized color_writes entries.
    let mut wide_device = LoweringInputs::new();
    wide_device
        .color_writes
        .insert(0, [Some(ChannelWrite::Device); 4]);
    let mut narrow_device = LoweringInputs::new();
    narrow_device
        .color_writes
        .insert(0, [Some(ChannelWrite::Device), None, None, None]);
    let c_wide = dev
        .compile_function(&fragment, &wide_device, &Budget::standard())
        .expect("wide device");
    let c_narrow = dev
        .compile_function(&fragment, &narrow_device, &Budget::standard())
        .expect("narrow device");
    assert_eq!(c_wide.key(), &dev.function_compile_key(&fragment, &wide_device));
    assert_eq!(c_wide.key(), c_narrow.key());
    assert_eq!(c_wide.artifact(), c_narrow.artifact());

    let mut with_extra = narrow_device.clone();
    with_extra
        .color_writes
        .insert(1, [Some(ChannelWrite::Selected); 4]);
    let c_extra = dev
        .compile_function(&fragment, &with_extra, &Budget::standard())
        .expect("extra color");
    assert_eq!(c_extra.key(), &dev.function_compile_key(&fragment, &with_extra));
}

#[test]
fn zeroed_color_attachments_stay_in_fragment_color_writes() {
    let Some(bc) = fixture("stage_io") else {
        return;
    };
    let dev = device("lavapipe");
    let lib = Library::from_air(&bc).expect("library");
    let d = pipeline(&lib, &[(PixelFormat::BGRA8Unorm, true)]);
    let mut variant = VariantKey::default();
    variant.zeroed_colors = BTreeMap::from([(1, PixelFormat::RGBA8Unorm)]);
    let keys = dev
        .render_variant_key(
            &d,
            &variant,
            &Budget::standard(),
            &crate::pipeline::StageEntries::new(),
        )
        .expect("variant keys");
    let inputs = keys.fragment_inputs.expect("fragment inputs");
    assert!(
        inputs.color_writes.contains_key(&1),
        "zeroed attachment 1 must keep its color_writes entry: {:?}",
        inputs.color_writes
    );
    dev.compile_render_variant(&d, &variant, &Budget::standard())
        .expect("zeroed color variant compiles and links");
}
