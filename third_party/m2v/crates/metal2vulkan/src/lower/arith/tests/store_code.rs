//! A unorm channel written for GM20B's store (`color_write::Lane::StoreCode`), on the interpreter:
//! the device bench's store rule makes every value written Metal's code, for every width
//! `NativeShaderTarget::stores_unorm_code` takes, beside one another in one output.

use super::*;
use crate::lower::arith::color_write::{self, Lane};

/// The code GM20B's ROP stores for `w` in an `n`-bit unorm channel: 0 for a NaN and from 0 down,
/// the maximum from 1 up, and otherwise `w` truncated to `n + 4` fractional bits, its product
/// with the maximum rounded to nearest with ties down.
fn rop_code(w: f32, n: u32) -> u32 {
    let m = (1u64 << n) - 1;
    if w.is_nan() || w <= 0.0 {
        return 0;
    }
    if w >= 1.0 {
        return m as u32;
    }
    let u = (f64::from(w) * f64::from(1u32 << (n + 4))).floor() as u64;
    ((u * m + (1 << (n + 3)) - 1) >> (n + 4)) as u32
}

/// RGB10A2's two-bit alpha: the top two bits of the 10-bit code the store makes.
fn rop_alpha2(w: f32) -> u32 {
    rop_code(w, 10) >> 8
}

/// Metal's code of `x` in a unorm channel of maximum `m`: NaN 0, clamped to `[0, 1]`, the exact
/// product rounded to nearest (its one tie, at 0.5, to even, which is away from zero there).
fn metal_code(x: f32, m: u32) -> u32 {
    let x = if x.is_nan() { 0.0 } else { x.clamp(0.0, 1.0) };
    (f64::from(x) * f64::from(m)).round_ties_even() as u32
}

const WIDTHS: [u8; 4] = [8, 10, 16, 2];

fn values() -> Vec<f32> {
    let mut out = vec![
        0.0,
        -0.0,
        0.5,
        1.0,
        -1.0,
        2.0,
        1e10,
        -1e10,
        f32::from_bits(1),
        f32::MIN_POSITIVE,
        f32::INFINITY,
        f32::NEG_INFINITY,
        f32::NAN,
        f32::from_bits(0xFFC0_0000),
    ];
    for m in [3u32, 255, 1023, 65535] {
        let step = (m / 1024).max(1) as usize;
        for k in (0..=m).step_by(step) {
            for c in [f64::from(k), f64::from(k) + 0.5] {
                let x = (c / f64::from(m)) as f32;
                for d in 0..=4 {
                    out.push(f32::from_bits(x.to_bits().wrapping_add(d)));
                    out.push(f32::from_bits(x.to_bits().wrapping_sub(d)));
                }
            }
        }
    }
    for h in 0u16..0x3C01 {
        out.push(f16_to_f64(h) as f32);
    }
    out
}

/// Run `lanes` on four-lane outputs of every case (`word`: the rule word input, where given) and
/// check each lane's value with `accept(lane, input, got)`.
fn run_lanes(
    what: &str,
    lanes: &[Option<Lane>; 4],
    cases: &[f32],
    word: Option<u32>,
    accept: &dyn Fn(usize, f32, f32) -> bool,
) {
    let f = vec(float(32), 4);
    let ins: Vec<Form> = match word {
        Some(_) => vec![f, int(32)],
        None => vec![f],
    };
    let m = kernel_with(
        &ins,
        f,
        &|fb, x| color_write::write(fb, f, x[0], lanes, (None, x.get(1).copied())),
        true,
        false,
    )
    .unwrap();
    let cases: Vec<Case> = cases
        .iter()
        .map(|x| {
            let mut c = vec![vec![u64::from(x.to_bits()); 4]];
            c.extend(word.map(|w| vec![u64::from(w)]));
            c
        })
        .collect();
    let got: Vec<_> = cases
        .chunks(CASES_PER_DISPATCH)
        .flat_map(|part| dispatch_module(what, &m, &ins, f, part))
        .collect();
    for (case, got) in cases.iter().zip(got) {
        let x = f32::from_bits(case[0][0] as u32);
        for (lane, &g) in got.iter().enumerate() {
            let g = f32::from_bits(g as u32);
            assert!(
                accept(lane, x, g),
                "{what}: {x:e} ({:#x}) lane {lane}: {g:e}",
                x.to_bits()
            );
        }
    }
}

#[test]
fn stored_values_are_metals_codes() {
    let lanes = WIDTHS.map(|bits| Some(Lane::StoreCode(bits)));
    run_lanes(
        "GM20B unorm store",
        &lanes,
        &values(),
        None,
        &|lane, x, w| {
            let bits = WIDTHS[lane];
            let code = match bits {
                2 => rop_alpha2(w),
                n => rop_code(w, u32::from(n)),
            };
            code == metal_code(x, (1 << bits) - 1)
        },
    );
}

/// A code's value is the one the exact rounding writes (`color_write::round`), so the forms differ
/// only where Metal clamps.
#[test]
fn in_range_values_match_the_rounding() {
    let lanes = WIDTHS.map(|bits| Some(Lane::StoreCode(bits)));
    let inside: Vec<f32> = values()
        .into_iter()
        .filter(|x| (0.0..=1.0).contains(x))
        .collect();
    run_lanes(
        "GM20B unorm store in [0, 1]",
        &lanes,
        &inside,
        None,
        &|lane, x, w| {
            let m = (1u32 << WIDTHS[lane]) - 1;
            w.to_bits() == (metal_code(x, m) as f32 * (1.0 / m as f32)).to_bits()
        },
    );
}

/// A lane whose rule word selects the 8-bit unorm rule writes the store form's value where the
/// device takes it (`Lane::Selected` with `store_code`).
#[test]
fn selected_unorm8_lanes_store_metals_codes() {
    use crate::pipeline::color_writes::{RULE_BITS, UNORM8_CODE};
    let word = (0..4).fold(0u32, |w, i| w | UNORM8_CODE << (RULE_BITS * i));
    let lanes = [Some(Lane::Selected {
        half: color_write::HalfLane::AsComputed,
        store_code: true,
    }); 4];
    run_lanes(
        "GM20B selected unorm8 store",
        &lanes,
        &values(),
        Some(word),
        &|_, x, w| rop_code(w, 8) == metal_code(x, 255),
    );
}

/// The half GM20B's ROP stores for `w` in a half-float channel (the bench loop's rop1): rounded
/// toward zero, a finite value past the largest stored as 65504, infinities and -0 kept, a NaN
/// stored as 0x7e00.
fn rop_half(w: f32) -> u16 {
    if w.is_nan() {
        return 0x7e00;
    }
    let sign = if w.is_sign_negative() { 0x8000 } else { 0 };
    let a = w.abs();
    if a.is_infinite() {
        return sign | 0x7c00;
    }
    if a >= 65504.0 {
        return sign | 0x7bff;
    }
    if a < 2f32.powi(-14) {
        return sign | (f64::from(a) * 2f64.powi(24)).floor() as u16;
    }
    let bits = a.to_bits();
    let exponent = (bits >> 23) as u16 + 15 - 127;
    sign | exponent << 10 | ((bits & 0x7f_ffff) >> 13) as u16
}

/// A `float` output's lane to a half-float channel written as computed (its NaN quiet, as the
/// SM53 target writes it) stores what the in-shader rounding toward zero stores.
#[test]
fn half_lanes_written_as_computed_store_the_rounded_half() {
    use crate::pipeline::color_writes;
    let mut cases = values();
    for h in 0x3c00u16..0x7c00 {
        let x = f16_to_f64(h) as f32;
        let above = f32::from_bits(x.to_bits() + 1);
        cases.extend([x, -x, above, -above]);
    }
    let signalling_nan = f32::from_bits(0x7fa0_0001);
    cases.extend([65504.0, 65519.0, 65520.0, 1e6, -65520.0, signalling_nan]);
    let computed = Some(Lane::NanAs(0x7fc0_0000));
    let rounded = Some(Lane::Rounded(color_writes::HALF));
    run_lanes(
        "GM20B half store",
        &[computed, rounded, computed, rounded],
        &cases,
        None,
        &|_, x, w| rop_half(w) == rop_half(x),
    );
}
