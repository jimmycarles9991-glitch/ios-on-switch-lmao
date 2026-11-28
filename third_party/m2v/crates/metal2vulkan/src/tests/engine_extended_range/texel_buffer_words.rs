//! A two-plane `RGB10A8_2P_XR10` texture buffer, held as its color words alone (`R32_UINT`), as
//! Metal reads and writes one over an IOSurface plane: on an Apple M4 Max under macOS 27.2.1 a
//! kernel's and a fragment function's write of (0.5, 0.25, -0.25, 0.75) store `0x27f80100` into
//! each word and change no other byte, and a read returns (0.5, 0.2510, -0.2510, 0).
//!
//! - The binder joins a kernel's texture-buffer slot bound to one to the variant
//!   (`VariantKey::extended_range`), views it as `R32_UINT` and binds no plane.
//! - On the CPU interpreter, a `float4` write of every row Apple's Metal stored into a two-plane
//!   texture (`planes_m4.txt`) stores Apple's word into the slot's `R32ui` image, and a read of each
//!   of Apple's words is Apple's color with alpha 0, written back without its top two bits.

use super::kernel_planes::apple;
use super::*;
use crate::pipeline::layout::BindingSource;

const PLANES: ExtendedRange = ExtendedRange::Planes { srgb: false };

/// A kernel writing `in[i]` (`float4`, buffer 0) to `texture_buffer<float, access::write>` slot 0
/// at element `i`.
const STORE: &str = r#"define void @k(ptr addrspace(1) %w, ptr addrspace(1) %in, i32 %i) {
  %x = zext i32 %i to i64
  %p = getelementptr inbounds <4 x float>, ptr addrspace(1) %in, i64 %x
  %v = load <4 x float>, ptr addrspace(1) %p, align 16
  call void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1) %w, i32 %i, <4 x float> %v, i32 2)
  ret void
}

declare void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1), i32, <4 x float>, i32)

!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture_buffer<float, write>", !"air.arg_name", !"w"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"in"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
"#;

/// A kernel reading element `i` of `texture_buffer<float, access::read_write>` slot 0 into
/// `out[i]` (`float4`, buffer 0) and writing it back.
const READ_BACK: &str = r#"define void @k(ptr addrspace(1) %t, ptr addrspace(1) %out, i32 %i) {
  %x = zext i32 %i to i64
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %r = call { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %rs, i32 %i, i32 3)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %q = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %x
  store <4 x float> %v, ptr addrspace(1) %q, align 16
  call void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1) %t, i32 %i, <4 x float> %v, i32 3)
  ret void
}

declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), ptr addrspace(2), i32, i32)
declare void @air.write_texture_buffer_1d.v4f32(ptr addrspace(1), i32, <4 x float>, i32)

!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.arg_type_name", !"texture_buffer<float, read_write>", !"air.arg_name", !"t"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
"#;

/// A kernel's state: texture slot 0 a texture buffer of `pf`, buffer 0.
fn buffer_state(pf: PixelFormat) -> BindState {
    let mut t = StageTable::default();
    t.textures.insert(
        0,
        BoundTexture::Buffer {
            pixel_format: pf,
            offset: 0,
            length: 65536,
            bytes_per_row: 65536,
            resource_id: None,
            swizzle: metal::TextureSwizzleChannels::DEFAULT,
        },
    );
    t.buffers.insert(
        0,
        BoundBuffer::Buffer {
            length: 65536,
            offset: 0,
            gpu_address: 0x1_0000,
            device_address: None,
            attribute_stride: None,
        },
    );
    let mut s = BindState::default();
    s.stages.insert(Stage::Kernel, t);
    s
}

/// The `Image Format` operands of `words`' `OpTypeImage`s.
fn image_formats(words: &[u32]) -> Vec<u32> {
    const OP_TYPE_IMAGE: u32 = 25;
    let mut out = Vec::new();
    let mut i = 5;
    while i < words.len() {
        let n = (words[i] >> 16) as usize;
        if words[i] & 0xFFFF == OP_TYPE_IMAGE {
            out.extend(words.get(i + 8).copied());
        }
        i += n.max(1);
    }
    out
}

/// SPIR-V's `R32ui` image format.
const FORMAT_R32UI: u32 = 33;

/// The binder joins a texture-buffer slot bound to an `RGB10A8_2P_XR10` texture buffer to the
/// variant, as its extended-range form, and views it as `R32_UINT` with no plane; the variant's plan
/// states the same key. On the raw GM20B path's device shape, for a slot written alone and one
/// read and written.
#[test]
fn a_two_plane_texture_buffer_binds_its_words_alone() {
    let mut planned = 0;
    for (what, module) in [("store", STORE), ("read back", READ_BACK)] {
        let Some(bc) = assemble(&format!("words {what}"), module) else {
            return;
        };
        let dev = super::kernel_planes::raw_device();
        let plain = pipeline(&dev, &bc, &LowerInputs::new()).expect("a texture-buffer kernel");
        let shape = buffer_state(PixelFormat::RGB10A82PXR10).shape();
        let plan = plain
            .plan(&dev, &shape)
            .unwrap_or_else(|e| panic!("{what}: {e}"));
        assert_eq!(
            plan.variant.extended_range,
            [(SOURCE, PLANES)].into(),
            "{what}"
        );
        assert!(plan.variant.two_plane_textures.is_empty(), "{what}");
        let key = plan.variant.lowering_inputs(Stage::Kernel);
        let variant = pipeline(&dev, &bc, &key).expect("the variant's inputs");
        let again = variant.plan(&dev, &shape).expect("variant plan");
        assert_eq!(again.variant, plan.variant, "{what}");
        let set0 = &variant.layout.sets[0];
        let mut views = Vec::new();
        for d in again.descriptors.iter().filter(|d| d.set == 0) {
            match &d.source {
                WriteSource::TexelBuffer { slot, format, .. } => {
                    views.push((set0.bindings[d.binding as usize].source, slot.index, *format))
                }
                WriteSource::Texture { .. } => panic!("{what}: a texture view {:?}", d.source),
                _ => {}
            }
        }
        assert_eq!(
            views,
            [(
                BindingSource::Texture(SOURCE),
                0,
                crate::vk::Format::R32_UINT
            )],
            "{what}"
        );
        planned += 1;
    }
    assert_eq!(planned, 2);
}

/// Resources of `n` one-thread threadgroups: buffer 0 holding `bytes`.
fn buffer_resources(plan: &Plan, bytes: Vec<u8>) -> Resources {
    let mut spec = BTreeMap::new();
    for axis in [Axis::X, Axis::Y, Axis::Z] {
        spec.insert(SpecKey::LocalSize(axis), 1);
    }
    spec.insert(SpecKey::FlatLocalSize, 1);
    Resources {
        buffers: vec![Buffer {
            slot: Some(ResourceSlot {
                stage: Stage::Kernel,
                kind: ResourceKind::Buffer,
                index: 0,
            }),
            address: None,
            bytes,
        }],
        push: vec![0u8; plan.push.push_range.map_or(0, |(_, end)| end as usize)],
        spec,
    }
}

/// A texture buffer of `n` elements at slot 0, its texels `texels(i)`.
fn buffer_image(n: u32, texels: impl Fn(u32) -> [u32; 4]) -> Image {
    let slot = ResourceSlot {
        stage: Stage::Kernel,
        kind: ResourceKind::Texture,
        index: 0,
    };
    let mut img = Image::new(slot, [n, 1, 1], 1, 1, 1);
    for (i, t) in (0u32..).zip(img.texels.iter_mut()) {
        *t = texels(i);
    }
    img
}

/// `m` over `n` one-thread threadgroups in both scheduling orders, which must agree: buffer 0 and
/// the texture buffer's texels.
fn run_buffer(
    m: &TypedModule,
    res: &Resources,
    img: Image,
    n: u32,
    what: &str,
) -> (Vec<u8>, Vec<[u32; 4]>) {
    let mut outs = Vec::new();
    for order in [Order::Forward, Order::Reverse] {
        let cfg = Config {
            workgroups: [n, 1, 1],
            order,
            ..Config::default()
        };
        let (o, images) = run_with_images(m, &cfg, res.clone(), vec![img.clone()])
            .unwrap_or_else(|e| panic!("{what}: interpreter: {e:?}"));
        let texels = images.into_iter().next().expect("image").texels;
        outs.push((o.buffers.into_iter().next().expect("buffer"), texels));
    }
    assert_eq!(outs[0], outs[1], "{what}: the scheduling orders disagree");
    outs.swap_remove(0)
}

/// Lowering inputs of slot 0 bound to an `RGB10A8_2P_XR10` texture buffer, as the binder states
/// them: its extended-range form alone.
fn words_inputs() -> LowerInputs {
    let mut inputs = LowerInputs::new();
    inputs.extended_range.insert(SOURCE, PLANES);
    inputs
}

/// Each `float4` write Apple's Metal stored into a two-plane texture stores Apple's color word
/// (bits 30 and 31 clear) into a texture buffer's `R32ui` image, which holds nothing else.
#[test]
fn texture_buffer_writes_store_apples_words() {
    let rows = apple("f");
    let Some(bc) = assemble("words store", STORE) else {
        return;
    };
    let p = super::kernel_planes::raw_device().profile().clone();
    let inputs = words_inputs();
    let a = validate_entry(&bc, "k", &inputs, &p, "words store");
    assert_eq!(image_formats(&a.module.words), [FORMAT_R32UI]);
    let (plan, m) =
        typed_module_with(&bc, "k", &p, &inputs).unwrap_or_else(|e| panic!("words store: {e}"));
    let n = rows.len() as u32;
    let bytes = rows
        .iter()
        .flat_map(|r| r[..4].iter().flat_map(|w| w.to_le_bytes()))
        .collect();
    let junk = buffer_image(n, |_| [0x5a5a_5a5a; 4]);
    let (_, texels) = run_buffer(&m, &buffer_resources(&plan, bytes), junk, n, "words store");
    for (r, t) in rows.iter().zip(&texels) {
        assert_eq!(t[0], r[4], "{r:x?}: {:#x}", t[0]);
    }
}

/// Each of Apple's words read through a texture buffer's `R32ui` image (whose second component is
/// 0) is Apple's color with alpha 0, and written back stores the word without its top two bits.
#[test]
fn texture_buffer_reads_are_apples_colors_with_alpha_zero() {
    let rows = apple("r");
    let Some(bc) = assemble("words read back", READ_BACK) else {
        return;
    };
    let p = super::kernel_planes::raw_device().profile().clone();
    let inputs = words_inputs();
    let a = validate_entry(&bc, "k", &inputs, &p, "words read back");
    assert_eq!(image_formats(&a.module.words), [FORMAT_R32UI]);
    let (plan, m) = typed_module_with(&bc, "k", &p, &inputs)
        .unwrap_or_else(|e| panic!("words read back: {e}"));
    let n = rows.len() as u32;
    let words = buffer_image(n, |i| [rows[i as usize][0], 0, 0, 1]);
    let res = buffer_resources(&plan, vec![0xab; 16 * n as usize]);
    let (out, texels) = run_buffer(&m, &res, words, n, "words read back");
    for (i, r) in (0u32..).zip(&rows) {
        let want = [r[2], r[3], r[4], 0];
        let got: [u32; 4] = std::array::from_fn(|c| lane(&out, false, i, c));
        assert_eq!(got, want, "{r:x?}");
        assert_eq!(texels[i as usize][0], r[0] & 0x3fff_ffff, "{r:x?}: written back");
    }
}

const READ_ONLY: &str = r#"define void @k(ptr addrspace(1) %t, ptr addrspace(1) %out, i32 %i) {
  %x = zext i32 %i to i64
  %rs = call ptr addrspace(2) @air.get_read_sampler()
  %r = call { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1) %t, ptr addrspace(2) %rs, i32 %i, i32 3)
  %v = extractvalue { <4 x float>, i8 } %r, 0
  %q = getelementptr inbounds <4 x float>, ptr addrspace(1) %out, i64 %x
  store <4 x float> %v, ptr addrspace(1) %q, align 16
  ret void
}

declare ptr addrspace(2) @air.get_read_sampler()
declare { <4 x float>, i8 } @air.read_texture_buffer_1d.v4f32(ptr addrspace(1), ptr addrspace(2), i32, i32)

!air.kernel = !{!0}
!0 = !{ptr @k, !1, !2}
!1 = !{}
!2 = !{!3, !4, !5}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.read", !"air.arg_type_name", !"texture_buffer<float, read>", !"air.arg_name", !"t"}
!4 = !{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"float4", !"air.arg_name", !"out"}
!5 = !{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"i"}
"#;

#[test]
fn single_plane_xr_texture_buffers_bind_and_store_apples_words() {
    use crate::vk::Format as F;
    let cases = [
        (
            PixelFormat::BGRA10XR,
            ExtendedRange::Bgra10 { srgb: false },
            F::R16G16B16A16_UINT,
            [0x4000u32, 0x8000, 0x9fc0, 0xbf80],
        ),
        (
            PixelFormat::BGRA10XRSRGB,
            ExtendedRange::Bgra10 { srgb: true },
            F::R16G16B16A16_UINT,
            [0x1b80u32, 0xa480, 0xbdc0, 0xbf80],
        ),
        (
            PixelFormat::BGR10XR,
            ExtendedRange::Bgr10 { srgb: false },
            F::R32_UINT,
            [0xe7f8_0100u32, 0, 0, 0],
        ),
        (
            PixelFormat::BGR10XRSRGB,
            ExtendedRange::Bgr10 { srgb: true },
            F::R32_UINT,
            [0xef7a_486eu32, 0, 0, 0],
        ),
    ];
    let dev = super::kernel_planes::raw_device();
    for (what, module) in [("store", STORE), ("read back", READ_BACK), ("read only", READ_ONLY)] {
        let Some(bc) = assemble(&format!("xr_tb {what}"), module) else {
            return;
        };
        let plain = pipeline(&dev, &bc, &LowerInputs::new()).expect("plain");
        for &(pf, form, raw_fmt, want_lanes) in &cases {
            let shape = buffer_state(pf).shape();
            let plan = plain
                .plan(&dev, &shape)
                .unwrap_or_else(|e| panic!("{what} {pf:?}: {e}"));
            assert_eq!(plan.variant.extended_range, [(SOURCE, form)].into(), "{what} {pf:?}");
            let inputs = plan.variant.lowering_inputs(Stage::Kernel);
            let variant = pipeline(&dev, &bc, &inputs).expect("variant");
            let again = variant.plan(&dev, &shape).expect("variant plan");
            assert_eq!(again.variant, plan.variant, "{what} {pf:?}");
            let views: Vec<_> = again
                .descriptors
                .iter()
                .filter_map(|d| match &d.source {
                    WriteSource::TexelBuffer { slot, format, .. } => Some((slot.index, *format)),
                    _ => None,
                })
                .collect();
            assert_eq!(views, [(0, raw_fmt)], "{what} {pf:?}");
            validate_entry(&bc, "k", &inputs, dev.profile(), &format!("{what} {pf:?}"));
            if what == "store" {
                let (plan, m) = typed_module_with(&bc, "k", dev.profile(), &inputs)
                    .unwrap_or_else(|e| panic!("module {pf:?}: {e}"));
                let bytes: Vec<u8> = [0.5f32, 0.25, -0.25, 0.75]
                    .iter()
                    .flat_map(|v| v.to_bits().to_le_bytes())
                    .collect();
                let junk = buffer_image(1, |_| [0x5a5a_5a5a; 4]);
                let (_, texels) = run_buffer(&m, &buffer_resources(&plan, bytes), junk, 1, "store");
                if raw_fmt == F::R32_UINT {
                    assert_eq!(texels[0][0], want_lanes[0], "{pf:?}: {:#x}", texels[0][0]);
                } else {
                    assert_eq!(texels[0], want_lanes, "{pf:?}: {:x?}", texels[0]);
                }
            }
        }
    }
}

#[test]
fn ycbcr_texture_buffers_bind_converted_uniform_texel_views() {
    use crate::vk::Format as F;
    let Some(bc) = assemble("ycbcr_tb", READ_ONLY) else {
        return;
    };
    let dev = super::kernel_planes::raw_device();
    let plain = pipeline(&dev, &bc, &LowerInputs::new()).expect("plain");
    for (pf, expected) in [
        (PixelFormat::YCBCR84202P, F::R16G16B16A16_SFLOAT),
        (PixelFormat::YCBCR84442P, F::R16G16B16A16_SFLOAT),
        (PixelFormat::RGB84202P, F::R8G8B8A8_UNORM),
    ] {
        let shape = buffer_state(pf).shape();
        let plan = plain
            .plan(&dev, &shape)
            .unwrap_or_else(|e| panic!("{pf:?}: {e}"));
        let views: Vec<_> = plan
            .descriptors
            .iter()
            .filter_map(|d| match &d.source {
                WriteSource::TexelBuffer { slot, format, .. } => Some((slot.index, *format)),
                _ => None,
            })
            .collect();
        assert_eq!(views, [(0, expected)], "{pf:?}");
    }
}
