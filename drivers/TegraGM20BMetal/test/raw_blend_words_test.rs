// SPDX-License-Identifier: MIT
#[allow(dead_code)]
#[path = "../../../third_party/m2v/crates/metal2vulkan/src/lower/arith/extended_range/blend_color.rs"]
mod blend_color;

use blend_color::constant_words;

#[test]
fn clamps_nan_infinity_and_alpha() {
    let values = constant_words([f32::NAN, f32::INFINITY, f32::NEG_INFINITY, f32::NAN]).map(f32::from_bits);
    assert_eq!(
        values,
        [
            -1542.0 / 2048.0,
            1710.0 / 1024.0,
            -1542.0 / 2048.0,
            0.0,
            1.0 + 1542.0 / 2048.0,
            1.0 - 1281.0 / 1024.0,
            1.0 + 1542.0 / 2048.0,
            1.0
        ]
    );
    assert_eq!(constant_words([0.0, 0.0, 0.0, f32::INFINITY])[3], 1.0f32.to_bits());
    assert_eq!(constant_words([0.0, 0.0, 0.0, f32::NEG_INFINITY])[3], 0);
}

#[test]
fn rounds_both_color_and_one_minus_as_metal_reads_them() {
    assert_eq!(
        constant_words([0.3, 1.2, -0.4, 0.45]),
        [
            0x3e99_a000,
            0x3f99_a000,
            0xbecc_c000,
            0x3ee6_6000,
            0x3f33_4000,
            0xbe4c_c000,
            0x3fb3_4000,
            0x3f0c_c000
        ]
    );
}

#[test]
fn distinguishes_the_srgb_direct_boundary() {
    let boundary = f32::from_bits(0x3fd5_9fd3);
    let below = f32::from_bits(boundary.to_bits() - 1);
    let values = constant_words([below, boundary, f32::from_bits(boundary.to_bits() + 1), 1.0]).map(f32::from_bits);
    assert_eq!(&values[..3], &[1709.0 / 1024.0, 1710.0 / 1024.0, 1710.0 / 1024.0]);
    assert_eq!(&values[4..7], &[1.0 - 1281.0 / 1024.0; 3]);
}

#[test]
fn half_ties_are_even_and_color_zero_is_positive() {
    for sign in [1.0, -1.0] {
        let mid = sign * (0.5 + 1.0 / 4096.0);
        let upper = sign * (0.5 + 3.0 / 4096.0);
        let values = constant_words([mid, upper, -0.0, -0.0]);
        assert_eq!(values[0], (sign * 0.5f32).to_bits());
        assert_eq!(values[1], (sign * (0.5f32 + 1.0 / 1024.0)).to_bits());
        assert_eq!(values[2], 0);
        assert_eq!(values[3], 0);
    }
}

#[test]
fn rounds_half_subnormal_midpoints() {
    let step = 2.0f32.powi(-24);
    let values = constant_words([step / 2.0, step * 1.5, step * 2.5, 0.0]);
    assert_eq!(values[0], 0);
    assert_eq!(values[1], (step * 2.0).to_bits());
    assert_eq!(values[2], (step * 2.0).to_bits());
}
