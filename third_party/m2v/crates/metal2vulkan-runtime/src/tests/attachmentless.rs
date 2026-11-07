use super::image_plan::committed;
use crate::attachmentless::keeps;
use crate::framebuffer_fetch::through_planes;
use crate::render_target::PassShape;
use metal2vulkan::vk::Format;

/// A pass of `samples` samples with `colors`, and a depth/stencil attachment where `depth`, whose
/// draws' subpass leaves out `storage`.
fn shape(samples: u32, colors: &[Option<Format>], depth: bool, storage: &[u32]) -> PassShape {
    PassShape {
        colors: colors.to_vec(),
        depth_stencil: depth.then_some(Format::D32_SFLOAT),
        samples,
        color_resolves: vec![None; colors.len()],
        depth_stencil_resolve: None,
        fetch_inputs: false,
        no_inputs: Vec::new(),
        storage: storage.to_vec(),
        view_mask: 0,
        locations: Vec::new(),
    }
}

/// A device keeps the pass of a shape without any attachment, at every sample count and view
/// mask (a multiview instance's), and of no other: not of one with a color or a depth/stencil
/// attachment, and not of the sample planes' pass, whose subpass leaves out every attachment the
/// pass still describes (it keeps its anchor instead, `render_target.rs`, on a device that reads
/// sample planes).
#[test]
fn only_a_pass_without_attachments_is_kept() {
    for samples in [1, 2, 4, 8] {
        assert!(keeps(&shape(samples, &[], false, &[])), "{samples}");
        for view_mask in [0b11, 0b1111] {
            let viewed = shape(samples, &[], false, &[]).with_view_mask(view_mask);
            assert!(keeps(&viewed), "{samples} {view_mask:#b}");
        }
    }
    assert!(!keeps(
        &shape(4, &[Some(Format::R32_SFLOAT)], false, &[]).with_view_mask(0b11)
    ));
    let float = Some(Format::R32_SFLOAT);
    assert!(!keeps(&shape(4, &[float], false, &[])));
    assert!(!keeps(&shape(4, &[], true, &[])));
    let planes = shape(4, &[float], false, &[0]);
    let (_, _, device) = committed()
        .into_iter()
        .find(|(_, _, device)| through_planes(device.profile(), 4))
        .expect("a committed profile that reads sample planes");
    assert_eq!(planes.anchor(device.profile()), Some(0));
    assert!(!keeps(&planes));
    let fetching = PassShape {
        fetch_inputs: true,
        ..shape(1, &[float], false, &[])
    };
    assert!(!keeps(&fetching));
}

/// The draws' subpass references an attachment at a location where its routing names a color
/// the pass has (each location its own color where it names none), and none at a color its
/// subpass leaves out: a pipeline keeps such a color unwritten there (`pipeline/graphics.rs`).
#[test]
fn a_routed_subpass_references_the_color_each_location_names() {
    let float = Some(Format::R8G8B8A8_UNORM);
    let own = shape(1, &[None, float], false, &[]);
    assert_eq!(
        (0..3).map(|l| own.references(l)).collect::<Vec<_>>(),
        [false, true, false]
    );
    let routed = shape(1, &[None, float], false, &[]).with_locations(vec![Some(1), None]);
    assert_eq!(
        (0..3).map(|l| routed.references(l)).collect::<Vec<_>>(),
        [true, false, false]
    );
    assert_eq!(routed.subpass_colors(), 2);
    let left_out = shape(1, &[float, float], false, &[1]);
    assert_eq!(
        (0..2).map(|l| left_out.references(l)).collect::<Vec<_>>(),
        [true, false]
    );
}

/// On a device that fetches through the pixel interlock alone, a render pass with a draw that
/// fetches its `BGRA8Unorm` color in the interlocked storage form and one whose pipeline the
/// compile layer left on input attachments (the color blended by the device, which the storage
/// form cannot keep exact) records the two in instances of different shapes: the interlocked
/// draw's subpass leaves the color out, the input fetch's references it as a color attachment and
/// reads it as its input attachment, leaving nothing out. A pass whose fetch reads input
/// attachments already keeps its shape.
#[test]
fn an_input_fetch_beside_the_interlock_records_in_an_instance_of_its_own() {
    let bgra = Some(Format::B8G8R8A8_UNORM);
    let interlocked = shape(1, &[bgra, bgra], false, &[0]);
    let inputs = shape(1, &[bgra, bgra], false, &[0]).with_inputs(vec![1]);
    assert!(!interlocked.fetch_inputs);
    assert!(inputs.fetch_inputs);
    assert!(inputs.storage.is_empty());
    assert_eq!(inputs.no_inputs, [1]);
    assert_ne!(interlocked, inputs);
    assert_eq!(
        (0..2)
            .map(|l| interlocked.references(l))
            .collect::<Vec<_>>(),
        [false, true]
    );
    assert_eq!(
        (0..2).map(|l| inputs.references(l)).collect::<Vec<_>>(),
        [true, true]
    );
    let already = PassShape {
        fetch_inputs: true,
        ..shape(1, &[bgra], false, &[])
    };
    assert_eq!(already.clone().with_inputs(vec![0]), already);
}
