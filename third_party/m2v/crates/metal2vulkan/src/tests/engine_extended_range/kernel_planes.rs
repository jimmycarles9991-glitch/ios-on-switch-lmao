//! A kernel's `RGB10A8_2P_XR10` texture held in Metal's two planes
//! (`VariantKey::two_plane_textures`): the slot's image holds the color words and its plane image
//! (`ResourceKind::TexturePlane`) the alpha bytes.
//!
//! - The binder joins a kernel slot bound to such a texture to the variant, views the slot's own
//!   image as `R32_UINT` and binds the plane as `R8_UINT`; a one-image texture keeps the
//!   `R32G32_UINT` view and no plane.
//! - On the CPU interpreter, a `float4` and a `half4` write of every row Apple's Metal stored
//!   (`planes_m4.txt`, an Apple M4 Max under macOS 27.2.1) stores Apple's word and byte into the
//!   two images, and the one-image form the same pair in one texel.
//! - A read through a sampled and a storage slot of every planted word and byte is Apple's value,
//!   and written back to a two-plane slot stores the word without its unread top bits and the
//!   byte.

use super::*;
use crate::pipeline::layout::BindingSource;

/// Apple's rows of kind `kind` in `planes_m4.txt`, each a row of hex words.
fn apple(kind: &str) -> Vec<Vec<u32>> {
    include_str!("planes_m4.txt")
        .lines()
        .filter(|l| l.split(' ').next() == Some(kind))
        .map(|l| {
            l.split(' ')
                .skip(1)
                .map(|x| u32::from_str_radix(x, 16).expect("hex"))
                .collect()
        })
        .collect()
}

/// A kernel writing `in[gid.x + 32 gid.y]` (`float4`, or `half4`) to `texture2d<T, access::write>`
/// slot 0 at `gid`.
fn store_kernel(half: bool) -> String {
    let (texel, vec4, size, in_ty, suffix) = if half {
        ("half", "<4 x half>", 8, "half4", "v4f16")
    } else {
        ("float", "<4 x float>", 16, "float4", "v4f32")
    };
    format!(
        r#"define void @k(ptr addrspace(1) %w, ptr addrspace(1) %in, <2 x i32> %gid) {{
  %x = extractelement <2 x i32> %gid, i64 0
  %y = extractelement <2 x i32> %gid, i64 1
  %row = shl i32 %y, 5
  %n = add i32 %x, %row
  %i = zext i32 %n to i64
  %p = getelementptr inbounds {vec4}, ptr addrspace(1) %in, i64 %i
  %v = load {vec4}, ptr addrspace(1) %p, align {size}
  call void @air.write_texture_2d.{suffix}(ptr addrspace(1) %w, <2 x i32> %gid, {vec4} %v, i32 0, i32 2)
  ret void
}}

declare void @air.write_texture_2d.{suffix}(ptr addrspace(1), <2 x i32>, {vec4}, i32, i32)

!air.kernel = !{{!0}}
!0 = !{{ptr @k, !1, !2}}
!1 = !{{}}
!2 = !{{!3, !4, !5}}
!3 = !{{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.write", !"air.arg_type_name", !"texture2d<{texel}, write>", !"air.arg_name", !"w"}}
!4 = !{{i32 1, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 {size}, !"air.arg_type_align_size", i32 {size}, !"air.arg_type_name", !"{in_ty}", !"air.arg_name", !"in"}}
!5 = !{{i32 2, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint2", !"air.arg_name", !"gid"}}
"#
    )
}

const PLANES: ExtendedRange = ExtendedRange::Planes { srgb: false };

/// The raw GM20B path's device shape (`two_plane::gm20b`), its attachments in one image and its
/// storage images formatless: no committed profile is vendored here.
fn gm20b() -> [(&'static str, Device); 1] {
    [("gm20b", super::two_plane::gm20b(false, true))]
}

/// The plane image of texture slot `index`, its texels `texels(n)`.
fn plane(index: u32, texels: impl Fn(u32) -> [u32; 4]) -> Image {
    let mut img = image(index, texels);
    img.slot.kind = ResourceKind::TexturePlane;
    img
}

/// The texels of the plane image of texture slot `index` among `images`.
fn plane_texels(images: &[Image], index: u32) -> &[[u32; 4]] {
    &images
        .iter()
        .find(|i| i.slot.kind == ResourceKind::TexturePlane && i.slot.index == index)
        .expect("plane image")
        .texels
}

/// The index in an image's texels of texel `n` of the 32 x 32 grid.
fn at(n: u32) -> usize {
    image(0, |_| [0; 4])
        .index(0, 0, 0, [n % SIDE, n / SIDE, 0])
        .expect("texel")
}

/// The binder joins a kernel slot bound to a two-plane `RGB10A8_2P_XR10` texture to the variant;
/// the variant's plan views the slot's image as `R32_UINT` and binds its plane as `R8_UINT`; a
/// texture of one image keeps the `R32G32_UINT` view, binds no plane and leaves the variant. On
/// the raw GM20B path's device shape, for a sampled and a storage source.
#[test]
fn a_two_plane_texture_joins_the_variant_and_binds_its_plane() {
    let mut planned = 0;
    for source in [Source::Read, Source::ReadWrite] {
        let Some(bc) = assemble("planes bind", &kernel(source, false, 0)) else {
            return;
        };
        for (stem, dev) in gm20b() {
            let Some(plain) = pipeline(&dev, &bc, &LowerInputs::new()) else {
                continue;
            };
            let what = format!("{source:?} on {stem}");
            let mut held = state(PixelFormat::RGB10A82PXR10);
            for table in held.stages.values_mut() {
                for t in table.textures.values_mut() {
                    if let BoundTexture::Texture { facts, .. } = t {
                        facts.two_planes = true;
                    }
                }
            }
            let shape = held.shape();
            let plan = match plain.plan(&dev, &shape) {
                Ok(plan) => plan,
                Err(crate::error::BindError::Unsupported(u)) if u.feature == Feature::Format => {
                    continue
                }
                Err(e) => panic!("{what}: {e}"),
            };
            assert_eq!(
                plan.variant.two_plane_textures,
                [SOURCE, TARGET].into(),
                "{what}"
            );
            let key = plan.variant.lowering_inputs(Stage::Kernel);
            assert_eq!(
                key.two_plane_textures, plan.variant.two_plane_textures,
                "{what}"
            );
            let variant = pipeline(&dev, &bc, &key).expect("the variant's inputs");
            let again = variant.plan(&dev, &shape).expect("variant plan");
            assert_eq!(again.variant, plan.variant, "{what}");
            let set0 = &variant.layout.sets[0];
            let mut views = Vec::new();
            for d in again.descriptors.iter().filter(|d| d.set == 0) {
                let WriteSource::Texture { slot, view } = &d.source else {
                    continue;
                };
                let source = set0.bindings[d.binding as usize].source;
                views.push((source, slot.index, view.format));
            }
            views.sort_by_key(|v| (v.0, v.1));
            assert_eq!(
                views,
                [
                    (
                        BindingSource::Texture(SOURCE),
                        0,
                        crate::vk::Format::R32_UINT
                    ),
                    (
                        BindingSource::Texture(TARGET),
                        1,
                        crate::vk::Format::R32_UINT
                    ),
                    (
                        BindingSource::TexturePlane(SOURCE),
                        0,
                        crate::vk::Format::R8_UINT
                    ),
                    (
                        BindingSource::TexturePlane(TARGET),
                        1,
                        crate::vk::Format::R8_UINT
                    ),
                ],
                "{what}"
            );
            let one = variant
                .plan(&dev, &state(PixelFormat::RGB10A82PXR10).shape())
                .expect("plan");
            assert!(one.variant.two_plane_textures.is_empty(), "{what}");
            assert_eq!(one.variant.extended_range.len(), 2, "{what}");
            planned += 1;
        }
    }
    assert!(planned > 0);
}

/// The kernel `bc` writing `rows` (four lanes each: `float` bits, or `half` bits) on `p` to slot 0,
/// two-plane where `planes`: each texel's word and byte as stored.
fn stored(
    bc: &[u8],
    p: &crate::device::profile::DeviceProfile,
    (rows, half, planes): (&[Vec<u32>], bool, bool),
    what: &str,
) -> Vec<(u32, u32)> {
    let mut inputs = LowerInputs::new();
    inputs.extended_range.insert(SOURCE, PLANES);
    if planes {
        inputs.two_plane_textures.insert(SOURCE);
    } else {
        let raw = StorageImageChoice::new(PLANES.raw_format(), StorageImageClass::Identity);
        inputs.storage_images.insert(SOURCE, raw);
    }
    validate(bc, &inputs, p, what);
    let (plan, m) =
        typed_module_with(bc, "k", p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
    let mut res = resources(&plan, 0, &[]);
    let n = (SIDE * SIDE) as usize;
    let bytes: Vec<u8> = (0..n)
        .flat_map(|i| {
            let row = &rows[i % rows.len()];
            row[..4]
                .iter()
                .flat_map(|&b| match half {
                    true => (b as u16).to_le_bytes().to_vec(),
                    false => b.to_le_bytes().to_vec(),
                })
                .collect::<Vec<u8>>()
        })
        .collect();
    res.buffers[0].bytes = bytes;
    let junk = |_| [0x5a5a_5a5a; 4];
    let mut images = vec![image(0, junk)];
    if planes {
        images.push(plane(0, junk));
    }
    let (_, images) = run(&m, &res, &images);
    let words = texels_of(&images, 0);
    (0..n as u32)
        .map(|k| {
            let w = words[at(k)];
            match planes {
                true => (w[0], plane_texels(&images, 0)[at(k)][0]),
                false => (w[0], w[1]),
            }
        })
        .collect()
}

/// Each `float4` and `half4` write Apple's Metal stored into a two-plane texture is Apple's word and
/// byte: through the two images of the two-plane form, and through the one image of the other.
#[test]
fn two_plane_kernel_writes_store_apples_bytes() {
    for (kind, half) in [("f", false), ("h", true)] {
        let rows = apple(kind);
        assert!(rows.len() >= 200, "{kind}: {} rows", rows.len());
        let Some(bc) = assemble("planes store", &store_kernel(half)) else {
            return;
        };
        for (stem, dev) in gm20b() {
            let p = dev.profile().clone();
            for planes in [true, false] {
                let what = format!("{kind} planes={planes} on {stem}");
                let got = stored(&bc, &p, (&rows, half, planes), &what);
                for (k, (word, byte)) in got.into_iter().enumerate() {
                    let row = &rows[k % rows.len()];
                    assert_eq!(
                        (word, byte),
                        (row[4], row[5]),
                        "{what}: row {k} {row:x?}: {word:#x} {byte:#x}"
                    );
                }
            }
        }
    }
}

/// Every word and byte of Apple's reads (`r` rows), read through a sampled and a storage slot of a
/// two-plane texture, is Apple's `float` value, and its `half` value Metal's rounding of it; written
/// back to a two-plane slot it stores the word without its top two bits and the byte.
#[test]
fn two_plane_reads_are_apples_values() {
    let rows = apple("r");
    assert_eq!(rows.len(), 128);
    for source in [Source::Read, Source::ReadWrite] {
        for half in [false, true] {
            let what = format!("planes {source:?} {}", if half { "half" } else { "float" });
            let Some(bc) = assemble(&what, &kernel(source, half, 0)) else {
                return;
            };
            for (stem, dev) in gm20b() {
                let p = dev.profile().clone();
                let what = format!("{what} on {stem}");
                let mut inputs = inputs(PLANES, source);
                inputs.two_plane_textures.extend([SOURCE, TARGET]);
                validate(&bc, &inputs, &p, &what);
                let (plan, m) = typed_module_with(&bc, "k", &p, &inputs)
                    .unwrap_or_else(|e| panic!("{what}: {e}"));
                let row = |n: u32| &rows[n as usize % rows.len()];
                let images = [
                    image(0, |n| [row(n)[0], 0xdead_beef, 0, 1]),
                    plane(0, |n| [row(n)[1] | 0x300, 0, 0, 1]),
                    image(1, |_| [0; 4]),
                    plane(1, |_| [0; 4]),
                ];
                let bytes = if half { 8 } else { 16 };
                let (out, images) = run(&m, &resources(&plan, bytes, &[]), &images);
                for n in 0..SIDE * SIDE {
                    let r = row(n);
                    for c in 0..4 {
                        let want = match half {
                            true => u32::from(half_bits(f32::from_bits(r[2 + c]))),
                            false => r[2 + c],
                        };
                        let got = lane(&out, half, n, c);
                        assert_eq!(got, want, "{what}: texel {n} channel {c} of {r:x?}");
                    }
                    assert_eq!(
                        (
                            texels_of(&images, 1)[at(n)][0],
                            plane_texels(&images, 1)[at(n)][0]
                        ),
                        (r[0] & 0x3fff_ffff, r[1]),
                        "{what}: texel {n} written back"
                    );
                }
            }
        }
    }
}

/// The nearest samples kernel `bc` takes on `dev` of a 64 x 2 two-plane texture holding Apple's
/// `r` rows, at each texel's center in pixel coordinates (a sampler of runtime word `word`, or a
/// constexpr one): each result's lanes as bits.
fn plane_samples(
    bc: &[u8],
    dev: &Device,
    rows: &[Vec<u32>],
    (half, word): (bool, Option<u32>),
    what: &str,
) -> Vec<[u32; 4]> {
    let p = dev.profile().clone();
    let mut inputs = LowerInputs::new();
    inputs.extended_range.insert(SOURCE, PLANES);
    inputs.two_plane_textures.insert(SOURCE);
    validate(bc, &inputs, &p, what);
    let (plan, m) =
        typed_module_with(bc, "k", &p, &inputs).unwrap_or_else(|e| panic!("{what}: {e}"));
    let mut spec = BTreeMap::new();
    for axis in [Axis::X, Axis::Y, Axis::Z] {
        spec.insert(SpecKey::LocalSize(axis), 1);
    }
    spec.insert(SpecKey::FlatLocalSize, 1);
    if let Some(word) = word {
        let s = plan
            .families
            .sampler
            .slots
            .get(&0)
            .expect("the runtime sampler");
        let fields = s.fields();
        spec.insert(
            SpecKey::SamplerState {
                slot: s.slot,
                fields,
            },
            u64::from(word & fields),
        );
    }
    let buffer = |index: u32, bytes: Vec<u8>| Buffer {
        slot: Some(ResourceSlot {
            stage: Stage::Kernel,
            kind: ResourceKind::Buffer,
            index,
        }),
        address: None,
        bytes,
    };
    let (width, n) = (64u32, rows.len() as u32);
    let coords: Vec<u8> = (0..n)
        .flat_map(|k| [(k % width) as f32 + 0.5, (k / width) as f32 + 0.5])
        .flat_map(f32::to_le_bytes)
        .collect();
    let size = if half { 8 } else { 16 };
    let res = Resources {
        buffers: vec![buffer(0, coords), buffer(1, vec![0xab; size * n as usize])],
        push: vec![0u8; plan.push.push_range.map_or(0, |(_, end)| end as usize)],
        spec,
    };
    let slot = |kind| ResourceSlot {
        stage: Stage::Kernel,
        kind,
        index: 0,
    };
    let mut words = Image::new(slot(ResourceKind::Texture), [width, n / width, 1], 1, 1, 1);
    let mut bytes = words.clone();
    bytes.slot = slot(ResourceKind::TexturePlane);
    for (k, r) in (0u32..).zip(rows) {
        let at = words
            .index(0, 0, 0, [k % width, k / width, 0])
            .expect("texel");
        words.texels[at] = [r[0], 0xdead_beef, 0, 1];
        bytes.texels[at] = [r[1] | 0x300, 0, 0, 1];
    }
    let mut outs = Vec::new();
    for order in [Order::Forward, Order::Reverse] {
        let cfg = Config {
            workgroups: [n, 1, 1],
            order,
            ..Config::default()
        };
        let (o, _) = run_with_images(&m, &cfg, res.clone(), vec![words.clone(), bytes.clone()])
            .unwrap_or_else(|e| panic!("{what}: interpreter: {e:?}"));
        outs.push(o.buffers.into_iter().nth(1).expect("out"));
    }
    assert_eq!(outs[0], outs[1], "{what}: the scheduling orders disagree");
    (0..n)
        .map(|k| std::array::from_fn(|c| lane(&outs[0], half, k, c)))
        .collect()
}

/// Each nearest sample of Apple's `r` rows through a two-plane texture's two images, by a
/// constexpr and a runtime sampler, is Apple's `float` sample, and its `half` sample Metal's
/// rounding of it: every tap joins the alpha byte from the plane image.
#[test]
fn two_plane_samples_are_apples_values() {
    use super::samples::{sample_kernel, word, words, Through};
    use crate::metal::{SamplerAddressMode, SamplerBorderColor};
    let rows = apple("r");
    let edge = SamplerAddressMode::ClampToEdge;
    let runtime = word(false, edge, true, SamplerBorderColor::TransparentBlack);
    for half in [false, true] {
        for (through, word) in [
            (Through::Constexpr(words(false, 1, true, 0)), None),
            (Through::Runtime, Some(runtime)),
        ] {
            let what = format!("planes sample {through:?} half={half}");
            let Some(bc) = assemble(&what, &sample_kernel(through, half, (0, 0), false)) else {
                return;
            };
            for (stem, dev) in gm20b() {
                let what = format!("{what} on {stem}");
                let got = plane_samples(&bc, &dev, &rows, (half, word), &what);
                for (r, g) in rows.iter().zip(got) {
                    let want: [u32; 4] = std::array::from_fn(|c| match half {
                        true => u32::from(half_bits(f32::from_bits(r[6 + c]))),
                        false => r[6 + c],
                    });
                    assert_eq!(g, want, "{what}: {r:x?}");
                }
            }
        }
    }
}

/// A sampled kernel slot bound to a two-plane texture joins the variant through a constexpr and
/// a runtime sampler (`ResourcePlanner::combined`): the variant's plan views the slot's image as
/// `R32_UINT`, binds its plane as `R8_UINT` and binds no sampler; a texture of one image leaves
/// the variant.
#[test]
fn a_sampled_two_plane_texture_joins_the_variant() {
    use super::samples::{sample_kernel, sampled_state, words, Through};
    use crate::bind::SamplerDescriptor;
    let mut planned = 0;
    for through in [
        Through::Constexpr(words(false, 1, true, 0)),
        Through::Runtime,
    ] {
        let Some(bc) = assemble(
            "planes sample bind",
            &sample_kernel(through, false, (0, 0), false),
        ) else {
            return;
        };
        for (stem, dev) in gm20b() {
            let what = format!("{through:?} on {stem}");
            let plain = pipeline(&dev, &bc, &LowerInputs::new()).expect("a sampling kernel");
            let mut held = sampled_state(PixelFormat::RGB10A82PXR10, SamplerDescriptor::new());
            if let Some(BoundTexture::Texture { facts, .. }) = held
                .stages
                .get_mut(&Stage::Kernel)
                .and_then(|t| t.textures.get_mut(&0))
            {
                facts.two_planes = true;
            }
            let shape = held.shape();
            let plan = plain
                .plan(&dev, &shape)
                .unwrap_or_else(|e| panic!("{what}: {e}"));
            assert_eq!(plan.variant.two_plane_textures, [SOURCE].into(), "{what}");
            let key = plan.variant.lowering_inputs(Stage::Kernel);
            let variant = pipeline(&dev, &bc, &key).expect("the variant's inputs");
            let again = variant.plan(&dev, &shape).expect("variant plan");
            assert_eq!(again.variant, plan.variant, "{what}");
            let set0 = &variant.layout.sets[0];
            let mut views = Vec::new();
            for d in again.descriptors.iter().filter(|d| d.set == 0) {
                match &d.source {
                    WriteSource::Texture { view, .. } => {
                        views.push((set0.bindings[d.binding as usize].source, view.format))
                    }
                    WriteSource::Sampler { .. } | WriteSource::CombinedImageSampler { .. } => {
                        panic!("{what}: the variant binds {:?}", d.source)
                    }
                    _ => {}
                }
            }
            views.sort_by_key(|v| v.0);
            assert_eq!(
                views,
                [
                    (BindingSource::Texture(SOURCE), crate::vk::Format::R32_UINT),
                    (
                        BindingSource::TexturePlane(SOURCE),
                        crate::vk::Format::R8_UINT
                    ),
                ],
                "{what}"
            );
            let one = sampled_state(PixelFormat::RGB10A82PXR10, SamplerDescriptor::new()).shape();
            let one = variant.plan(&dev, &one).expect("plan");
            assert!(one.variant.two_plane_textures.is_empty(), "{what}");
            planned += 1;
        }
    }
    assert_eq!(planned, 2);
}
