//! The work host calls stage for the device's next submission (`texture/first_move.rs`).

use crate::texture::first_move::{staged_barriers, FirstMoves, TexelCopy};
use ash::vk;
use ash::vk::Handle;
use std::any::Any;
use std::sync::{Arc, Weak};

/// A host call's copy staged into an image makes a later CPU copy of its texels wait for the
/// submission that makes it; a decode alone staged (`TexelCopy::decode_only`: of what a copy on
/// the host or the staging queue wrote into a BC image, into its decoded twin) does not, as it
/// writes the twin alone (`FirstMoves::texels_wait`).
#[test]
fn a_staged_decode_alone_leaves_the_texels_to_cpu_copies() {
    let firsts = FirstMoves::default();
    let decoding: Arc<u32> = Arc::new(0);
    let copied: Arc<u32> = Arc::new(1);
    let keeper = |image: &Arc<u32>| -> Weak<dyn Any + Send + Sync> {
        let weak: Weak<u32> = Arc::downgrade(image);
        weak
    };
    let at = |image: &Arc<u32>| Arc::as_ptr(image).cast::<()>();
    // A decode's shape, its pass aside: no copy of texels.
    let alone = TexelCopy::new(vk::Buffer::null(), 0, Vec::new(), Box::new(()));
    assert!(!firsts.register_texels(keeper(&decoding), alone));
    let copy = (vk::Image::null(), vk::BufferImageCopy::default());
    let staged = TexelCopy::new(vk::Buffer::null(), 16, vec![copy], Box::new(()));
    assert!(!firsts.register_texels(keeper(&copied), staged));
    assert!(!firsts.texels_wait(at(&decoding)));
    assert!(firsts.texels_wait(at(&copied)));
}

/// A copy of a `width` x `height` box at (`x`, `y`) of `level`, layer `layer`, `aspect`.
fn region(
    aspect: vk::ImageAspectFlags,
    level: u32,
    layer: u32,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
) -> vk::BufferImageCopy {
    vk::BufferImageCopy {
        image_subresource: vk::ImageSubresourceLayers {
            aspect_mask: aspect,
            mip_level: level,
            base_array_layer: layer,
            layer_count: 1,
        },
        image_offset: vk::Offset3D { x, y, z: 0 },
        image_extent: vk::Extent3D {
            width,
            height,
            depth: 1,
        },
        ..Default::default()
    }
}

/// A staged copy of `region` into `image`.
fn staged_into(image: u64, region: vk::BufferImageCopy) -> TexelCopy {
    TexelCopy::new(
        vk::Buffer::null(),
        16,
        vec![(vk::Image::from_raw(image), region)],
        Box::new(()),
    )
}

/// A staged copy into `image` of a colour box of `level`, layer 0.
fn staged_copy(image: u64, level: u32, x: i32, y: i32, width: u32, height: u32) -> TexelCopy {
    staged_into(
        image,
        region(vk::ImageAspectFlags::COLOR, level, 0, x, y, width, height),
    )
}

fn barriers(staged: &[TexelCopy]) -> Vec<bool> {
    staged_barriers(&staged.iter().collect::<Vec<_>>())
}

/// The first staged copy waits for the writes before it; copies into disjoint texels of one
/// image (a texture's levels and tiles, one `replace_region` each) need nothing between them.
#[test]
fn staged_copies_into_disjoint_texels_share_the_first_barrier() {
    let staged = [
        staged_copy(1, 0, 0, 0, 64, 64),
        staged_copy(1, 0, 64, 0, 64, 64),
        staged_copy(1, 1, 0, 0, 32, 32),
        staged_copy(1, 2, 0, 0, 16, 16),
    ];
    assert_eq!(barriers(&staged), [true, false, false, false]);
    let alone: Vec<TexelCopy> = [
        staged_copy(1, 0, 0, 0, 64, 64),
        staged_copy(1, 0, 0, 64, 64, 64),
    ]
    .into_iter()
    .map(TexelCopy::alone)
    .collect();
    assert_eq!(barriers(&alone), [true, false]);
}

/// A later copy into texels an earlier one wrote waits for it, as Metal's later call wins; the
/// barrier covers what came before it, so a third copy over the first texels needs none more.
#[test]
fn a_staged_copy_over_written_texels_waits_for_it() {
    let staged = [
        staged_copy(1, 0, 0, 0, 64, 64),
        staged_copy(1, 0, 32, 32, 64, 64),
        staged_copy(1, 1, 0, 0, 8, 8),
        staged_copy(1, 0, 0, 0, 8, 8),
    ];
    assert_eq!(barriers(&staged), [true, true, false, false]);
}

/// Boxes that only touch at an edge share no texel.
#[test]
fn staged_copies_whose_boxes_touch_share_no_texel() {
    let staged = [
        staged_copy(1, 0, 0, 0, 4, 4),
        staged_copy(1, 0, 4, 0, 4, 4),
        staged_copy(1, 0, 0, 4, 4, 4),
    ];
    assert_eq!(barriers(&staged), [true, false, false]);
}

/// Images that may alias (a heap's, a buffer's) are kept in order; an image in memory of its own
/// aliases none, so copies into it run beside any other image's.
#[test]
fn staged_copies_into_images_that_may_alias_keep_their_order() {
    let shared = [staged_copy(1, 0, 0, 0, 4, 4), staged_copy(2, 0, 0, 0, 4, 4)];
    assert_eq!(barriers(&shared), [true, true]);
    let one_alone = [
        staged_copy(1, 0, 0, 0, 4, 4),
        staged_copy(2, 0, 0, 0, 4, 4).alone(),
    ];
    assert_eq!(barriers(&one_alone), [true, false]);
    let both_alone = [
        staged_copy(1, 0, 0, 0, 4, 4).alone(),
        staged_copy(2, 0, 0, 0, 4, 4).alone(),
    ];
    assert_eq!(barriers(&both_alone), [true, false]);
}

/// Copies of one layer each, or of one aspect each, write different texels.
#[test]
fn staged_copies_of_other_layers_or_aspects_share_no_texel() {
    let layer = staged_into(1, region(vk::ImageAspectFlags::COLOR, 0, 1, 0, 0, 4, 4));
    let depth = vk::ImageAspectFlags::DEPTH;
    let stencil = vk::ImageAspectFlags::STENCIL;
    assert_eq!(
        barriers(&[staged_copy(1, 0, 0, 0, 4, 4), layer]),
        [true, false]
    );
    let aspects = [
        staged_into(2, region(depth, 0, 0, 0, 0, 4, 4)),
        staged_into(2, region(stencil, 0, 0, 0, 0, 4, 4)),
        staged_into(2, region(depth | stencil, 0, 0, 0, 0, 4, 4)),
    ];
    assert_eq!(barriers(&aspects), [true, false, true]);
}
