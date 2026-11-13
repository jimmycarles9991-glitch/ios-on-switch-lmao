//! `RGB10A8_2P_XR10` held in Metal's two planes (`VkProperty::TwoPlaneExtendedRange`,
//! `pipeline::two_plane`):
//!
//! - the alpha index is the lowest one the descriptor leaves free, taken in index order, and an
//!   attachment past the last free index is refused (`NotImplemented(Format)`);
//! - a fragment function's output is split: its color word through a `uint` variable at the
//!   attachment's index, its alpha byte through one at the alpha index, which link gives `R32_UINT`
//!   and `R8_UINT`, each written under a full mask and neither under an empty one, no blend;
//! - an input attachment's read (unordered, the raw path's barrier form) reads both planes, the
//!   alpha's at the alpha index;
//! - the storage form (the pixel interlock) reads and writes both planes, the word as `R32_UINT`
//!   and the byte as `R8_UINT`;
//! - a profile without the property compiles the one-image form as before.

use super::*;
use crate::error::{Error, Needs};
use crate::metal::ColorWriteMask;
use crate::pipeline::descriptor::ColorAttachmentDescriptor;
use crate::pipeline::link::{link, LinkInputs, RenderLinkInputs};
use crate::pipeline::relocate::relocate;
use crate::pipeline::stage_io::{IoNumeric, IoRole};
use crate::pipeline::testing::{compile, compile_with, fragment_inputs};
use crate::spirv::testing::{check_words_valid, spirv_dis};
use crate::vk;

/// A vertex function and two fragment functions: `xr_fill` returns its position as a `half4` to color 0,
/// `xr_fetch` adds its position to the `float4` it reads of color 0 and returns that there
const MODULE: &str = r#"
define <4 x float> @xr_vertex(i32 %vid) {
entry:
  %lo = and i32 %vid, 1
  %x = uitofp i32 %lo to float
  %v = insertelement <4 x float> <float poison, float 0.0, float 0.5, float 1.0>, float %x, i64 0
  ret <4 x float> %v
}

define <4 x half> @xr_fill(<4 x float> %pos) {
entry:
  %h = fptrunc <4 x float> %pos to <4 x half>
  ret <4 x half> %h
}

define <4 x float> @xr_fetch(<4 x float> %pos, <4 x float> %previous) {
entry:
  %v = fadd <4 x float> %pos, %previous
  ret <4 x float> %v
}

!air.vertex = !{!0}
!air.fragment = !{!5, !10}
!0 = !{ptr @xr_vertex, !1, !3}
!1 = !{!2}
!2 = !{!"air.position", !"air.arg_type_name", !"float4"}
!3 = !{!4}
!4 = !{i32 0, !"air.vertex_id", !"air.arg_type_name", !"uint", !"air.arg_name", !"vid"}
!5 = !{ptr @xr_fill, !6, !8}
!6 = !{!7}
!7 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"half4"}
!8 = !{!9}
!9 = !{i32 0, !"air.position", !"air.center", !"air.no_perspective", !"air.arg_type_name", !"float4", !"air.arg_name", !"position"}
!10 = !{ptr @xr_fetch, !11, !13}
!11 = !{!12}
!12 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!13 = !{!9, !14}
!14 = !{i32 1, !"air.render_target", i32 0, !"air.arg_type_name", !"float4", !"air.arg_name", !"previous"}
!air.compile_options = !{!15}
!15 = !{!"air.compile.framebuffer_fetch_enable"}
"#;

/// [`MODULE`] assembled by `llvm-as`, or `None` (skipped visibly) without it
fn module() -> Option<Vec<u8>> {
    let dir = std::env::temp_dir().join(format!("m2v-two-plane-{}-{:?}", std::process::id(), std::thread::current().id()));
    std::fs::create_dir_all(&dir).expect("scratch");
    let path = dir.join("two_plane.ll");
    std::fs::write(&path, format!("{HEADER}{MODULE}")).expect("write");
    let assembled = llvm_assemble(&path);
    let _ = std::fs::remove_dir_all(&dir);
    match assembled {
        Ok(b) => Some(b),
        Err(AssembleError::Missing(why)) => {
            eprintln!("SKIPPED: {why}");
            None
        }
        Err(AssembleError::Rejected(why)) => panic!("llvm-as rejected the two-plane module: {why}"),
    }
}

/// A device of the raw GM20B path's shape (no committed profile is vendored): every format the translator asks about
/// supports everything, and its framebuffer fetch is the pixel interlock (`interlock`) or unordered input attachments,
/// as the raw path's storage and barrier forms compile; held in two planes where `planes`
fn device(interlock: bool, planes: bool) -> crate::device::profile::DeviceProfile {
    use crate::device::description::FormatProperties;
    use crate::device::profile::Limits;
    use crate::device::FramebufferFetchPolicy;
    use crate::device::requirements::{ApiVersion, VkExtension, VkFeature};
    let mut desc = DeviceDescription::new(ApiVersion::V1_3);
    desc.limits = Limits {
        max_compute_work_group_size: [1024, 1024, 64],
        max_compute_work_group_count: [65535; 3],
        max_compute_work_group_invocations: 1024,
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
    ]);
    let mut policy = DevicePolicy::default();
    if interlock {
        desc.extensions.insert(VkExtension::ExtFragmentShaderInterlock);
        desc.features.insert(VkFeature::FragmentShaderPixelInterlock);
        policy.framebuffer_fetch = FramebufferFetchPolicy::RequireOrdered;
    } else {
        policy.framebuffer_fetch = FramebufferFetchPolicy::AcceptUnordered;
    }
    let made = Device::new(desc, policy).expect("test device");
    match planes {
        true => made.with_two_plane_extended_range().profile().clone(),
        false => made.profile().clone(),
    }
}

/// Both forms' devices, named
fn forms(planes: bool) -> [(&'static str, crate::device::profile::DeviceProfile); 2] {
    [("interlock", device(true, planes)), ("inputs", device(false, planes))]
}

/// A render pipeline of `formats`' color attachments, each written under `mask`
fn pipeline_of(formats: &[Option<PixelFormat>], mask: ColorWriteMask) -> RenderLinkInputs {
    let mut r = RenderLinkInputs::new();
    for (i, f) in formats.iter().enumerate() {
        r.color_attachments[i] = f.map(|pf| {
            let mut a = ColorAttachmentDescriptor::new(pf);
            a.write_mask = mask;
            a
        });
    }
    r
}

fn render(r: &RenderLinkInputs) -> LinkInputs {
    LinkInputs::Render(Box::new(r.clone()))
}

const XR: Option<PixelFormat> = Some(PixelFormat::RGB10A82PXR10);
const BGRA: Option<PixelFormat> = Some(PixelFormat::BGRA8Unorm);

#[test]
fn the_alpha_index_is_the_lowest_free_one() {
    let p = device(false, true);
    let alphas = |formats: &[Option<PixelFormat>]| {
        crate::pipeline::two_plane::alphas(&pipeline_of(formats, ColorWriteMask::ALL).color_attachments, &p)
    };
    assert_eq!(alphas(&[XR]), BTreeMap::from([(0, 1)]));
    assert_eq!(alphas(&[XR, XR]), BTreeMap::from([(0, 2), (1, 3)]));
    assert_eq!(alphas(&[XR, BGRA, XR]), BTreeMap::from([(0, 3), (2, 4)]));
    assert_eq!(alphas(&[BGRA, XR]), BTreeMap::from([(1, 2)]));
    assert_eq!(alphas(&[XR, None, BGRA]), BTreeMap::from([(0, 1)]));
    assert_eq!(alphas(&[XR, BGRA, BGRA, BGRA, BGRA, BGRA, BGRA, BGRA]), BTreeMap::new());
    let plain = device(false, false);
    assert!(crate::pipeline::two_plane::alphas(
        &pipeline_of(&[XR], ColorWriteMask::ALL).color_attachments,
        &plain
    )
    .is_empty());
}

#[test]
fn outputs_are_split_into_the_planes() {
    let Some(bc) = module() else {
        return;
    };
    for (stem, p) in forms(true) {
        let v = compile(&bc, "xr_vertex", &p).expect("vertex");
        for entry in ["xr_fill"] {
            for (mask, written) in [(ColorWriteMask::ALL, true), (ColorWriteMask::NONE, false)] {
                let what = format!("{stem} {entry} {mask:?}");
                let r = pipeline_of(&[XR, None, BGRA], mask);
                let inputs = fragment_inputs(&r, &p);
                assert_eq!(inputs.two_plane_alphas, BTreeMap::from([(0, 1)]), "{what}");
                let f = compile_with(&bc, entry, &p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
                assert_eq!(f.interface.io.two_plane_alphas, BTreeMap::from([(0, 1)]), "{what}");
                let alpha = f.interface.io.outputs.iter().find(|o| {
                    matches!(o.role, IoRole::Color { attachment: 1, index: 0 })
                });
                assert_eq!(
                    alpha.map(|o| (o.ty.numeric, o.ty.components)),
                    Some((IoNumeric::Uint, 1)),
                    "{what}"
                );
                let l = link(&[&v, &f], &render(&r), &p).unwrap_or_else(|e| panic!("{what}: link: {e}"));
                let g = l.graphics.as_ref().expect("graphics");
                use vk::ColorComponentFlags as C;
                let mask = if written { C::R } else { C::empty() };
                let word = g.color_attachments[0].expect("word");
                let byte = g.color_attachments[1].expect("alpha");
                assert_eq!((word.format, word.write_mask), (vk::Format::R32_UINT, mask), "{what}");
                assert_eq!((byte.format, byte.write_mask), (vk::Format::R8_UINT, mask), "{what}");
                assert!(word.blend.is_none() && byte.blend.is_none(), "{what}");
                assert_eq!(g.color_outputs & 0b11, 0b11, "{what}");
                let words = relocate(&f, &l.table).expect("relocate");
                check_words_valid(&words, &f.requirements, &p).unwrap_or_else(|e| panic!("{what}: L3: {e}"));
                let text = spirv_dis(&words);
                assert!(text.contains("%_ptr_Output_uint = OpTypePointer Output %uint"), "{what}\n{text}");
                assert!(!text.contains("OpTypePointer Output %v2uint"), "{what}\n{text}");
            }
        }
        // Every index taken: no alpha index, refused.
        let r = pipeline_of(&[XR, BGRA, BGRA, BGRA, BGRA, BGRA, BGRA, BGRA], ColorWriteMask::ALL);
        let e = compile_with(&bc, "xr_fill", &p, &fragment_inputs(&r, &p)).expect_err("no alpha index");
        assert!(
            matches!(&e, Error::Unsupported(u) if u.feature == Feature::Format && u.needs == Needs::NotImplemented),
            "{stem}: {e}"
        );
    }
}

/// The one-image form on a profile without the property: the attachment `R32G32_UINT`
#[test]
fn a_profile_without_the_planes_holds_one_image() {
    let Some(bc) = module() else {
        return;
    };
    let p = device(false, false);
    let v = compile(&bc, "xr_vertex", &p).expect("vertex");
    let r = pipeline_of(&[XR], ColorWriteMask::ALL);
    let f = compile_with(&bc, "xr_fill", &p, &fragment_inputs(&r, &p)).expect("fragment");
    assert!(f.interface.io.two_plane_alphas.is_empty());
    let l = link(&[&v, &f], &render(&r), &p).expect("link");
    let g = l.graphics.as_ref().expect("graphics");
    assert_eq!(g.color_attachments[0].expect("attachment").format, vk::Format::R32G32_UINT);
    assert_eq!(g.color_attachments.iter().flatten().count(), 1);
}

/// A fetch of the attachment reads both planes: through two input attachments on a device that
/// reads them unordered (the raw path's barrier form), through two storage images in the pixel
/// interlock (its storage form)
#[test]
fn fetches_read_and_write_both_planes() {
    let Some(bc) = module() else {
        return;
    };
    for (stem, p) in forms(true) {
        let v = compile(&bc, "xr_vertex", &p).expect("vertex");
        let r = pipeline_of(&[XR], ColorWriteMask::ALL);
        let mut inputs = fragment_inputs(&r, &p);
        if crate::lower::fragment::interlock_orders(&p) {
            inputs.framebuffer_fetch = Some(crate::pipeline::FramebufferFetchInputs::of(
                [0],
                &r.color_attachments,
                false,
                false,
                1,
            ));
        }
        let what = format!("{stem} fetch");
        let f = compile_with(&bc, "xr_fetch", &p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
        assert_eq!(f.interface.io.two_plane_alphas, BTreeMap::from([(0, 1)]), "{what}");
        let l = link(&[&v, &f], &render(&r), &p).unwrap_or_else(|e| panic!("{what}: link: {e}"));
        let g = l.graphics.as_ref().expect("graphics");
        if g.storage_attachments.is_empty() {
            let read: Vec<u32> = g.input_attachments.iter().map(|i| i.color_index).collect();
            assert_eq!(read, [0, 1], "{what}");
        } else {
            let views: Vec<(u32, vk::Format)> =
                g.storage_attachments.iter().map(|s| (s.color_index, s.format)).collect();
            assert_eq!(views, [(0, vk::Format::R32_UINT), (1, vk::Format::R8_UINT)], "{what}");
        }
        let words = relocate(&f, &l.table).expect("relocate");
        check_words_valid(&words, &f.requirements, &p).unwrap_or_else(|e| panic!("{what}: L3: {e}"));
    }
}

/// A source-over blend of the attachment reads and writes both planes: in the pixel interlock
/// through two storage images (the raw path's storage form), through two input attachments where
/// the device blends over them; neither plane is blended by the ROP
#[test]
fn blends_read_and_write_both_planes() {
    let Some(bc) = module() else {
        return;
    };
    for (stem, p) in forms(true) {
        let v = compile(&bc, "xr_vertex", &p).expect("vertex");
        let mut r = pipeline_of(&[XR], ColorWriteMask::ALL);
        let a = r.color_attachments[0].as_mut().expect("attachment");
        a.blending_enabled = true;
        a.source_rgb_blend_factor = crate::metal::BlendFactor::One;
        a.source_alpha_blend_factor = crate::metal::BlendFactor::One;
        a.destination_rgb_blend_factor = crate::metal::BlendFactor::OneMinusSourceAlpha;
        a.destination_alpha_blend_factor = crate::metal::BlendFactor::OneMinusSourceAlpha;
        let mut inputs = fragment_inputs(&r, &p);
        let interlock = crate::lower::fragment::interlock_orders(&p);
        inputs.framebuffer_fetch = Some(crate::pipeline::FramebufferFetchInputs::of(
            [0],
            &r.color_attachments,
            false,
            false,
            1,
        ));
        let what = format!("{stem} blend");
        let f = compile_with(&bc, "xr_fill", &p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
        assert_eq!(f.interface.io.two_plane_alphas, BTreeMap::from([(0, 1)]), "{what}");
        let l = link(&[&v, &f], &render(&r), &p).unwrap_or_else(|e| panic!("{what}: link: {e}"));
        let g = l.graphics.as_ref().expect("graphics");
        assert!(g.color_attachments.iter().flatten().all(|c| c.blend.is_none()), "{what}");
        if interlock {
            let views: Vec<(u32, vk::Format)> =
                g.storage_attachments.iter().map(|s| (s.color_index, s.format)).collect();
            assert_eq!(views, [(0, vk::Format::R32_UINT), (1, vk::Format::R8_UINT)], "{what}");
        } else {
            let attachments: Vec<vk::Format> = g.color_attachments.iter().flatten().map(|c| c.format).collect();
            assert_eq!(attachments, [vk::Format::R32_UINT, vk::Format::R8_UINT], "{what}");
            let read: Vec<u32> = g.input_attachments.iter().map(|i| i.color_index).collect();
            assert_eq!(read, [0, 1], "{what}");
        }
        let words = relocate(&f, &l.table).expect("relocate");
        check_words_valid(&words, &f.requirements, &p).unwrap_or_else(|e| panic!("{what}: L3: {e}"));
    }
}
