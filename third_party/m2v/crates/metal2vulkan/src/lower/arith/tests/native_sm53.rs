use super::*;
use crate::device::requirements::{ApiVersion, VkFeature};
use crate::device::{Device, DeviceDescription, DevicePolicy, NativeShaderTarget};
use crate::spirv::verify::{self, VerifyError, Violation};

fn profile() -> DeviceProfile {
    use crate::device::profile::Limits;
    let mut desc = DeviceDescription::new(ApiVersion::V1_3);
    desc.limits = Limits {
        max_compute_work_group_size: [1024, 1024, 64],
        max_compute_work_group_count: [65535; 3],
        max_compute_work_group_invocations: 1024,
        max_push_constants_size: 128,
        max_bound_descriptor_sets: 4,
        max_per_stage_descriptor_storage_buffers: 32,
        max_per_stage_descriptor_uniform_buffers: 32,
        max_per_stage_resources: 64,
        max_descriptor_set_storage_buffers: 32,
        max_descriptor_set_uniform_buffers: 32,
        max_storage_buffer_range: 1 << 27,
        max_uniform_buffer_range: 65536,
        ..Limits::default()
    };
    Device::new(desc, DevicePolicy::default())
        .unwrap()
        .with_native_target(NativeShaderTarget::Sm53)
        .profile()
        .clone()
}

fn check(
    what: &str,
    ins: &[Form],
    out: Form,
    body: Body<'_>,
    cases: &[Case],
    reference: &dyn Fn(&Case) -> Option<Vec<u64>>,
    loose_zero: bool,
) {
    let m = kernel_with(ins, out, body, true, false).unwrap();
    verify_native(&m).unwrap();
    let got: Vec<_> = cases
        .chunks(CASES_PER_DISPATCH)
        .flat_map(|part| dispatch_module(what, &m, ins, out, part))
        .collect();
    for (case, got) in cases.iter().zip(got) {
        let want = reference(case).unwrap();
        assert!(
            got.iter().zip(&want).all(|(&g, &w)| g == w
                || (is_nan(out, g) && is_nan(out, w))
                || (loose_zero && is_zero(out, g) && is_zero(out, w))),
            "{what}: {case:x?}: {got:x?} != {want:x?}"
        );
    }
}

fn verify_native(m: &TypedModule) -> Result<verify::VerifyReport, VerifyError> {
    let budget = crate::Budget::standard();
    verify::check(m, &profile(), &mut crate::budget::Meter::new(&budget))
}

fn cases(lanes: u8) -> Vec<Case> {
    let edge = [
        0, 0x8000, 1, 0x8001, 0x03ff, 0x83ff, 0x0400, 0x8400, 0x3555, 0xb555, 0x3bff, 0x3c00,
        0x3c01, 0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0xfe00, 0x7c01, 0xffff,
    ];
    let mut triples = Vec::new();
    for &x in &edge {
        for &y in &edge {
            for &z in &edge {
                triples.push([x, y, z]);
            }
        }
    }
    for x in (0..=u16::MAX).step_by(7) {
        triples.push([x, x.wrapping_mul(40503), x.wrapping_mul(7919)]);
    }
    triples
        .chunks(usize::from(lanes))
        .map(|chunk| {
            (0..3)
                .map(|operand| {
                    (0..usize::from(lanes))
                        .map(|lane| u64::from(chunk[lane % chunk.len()][operand]))
                        .collect()
                })
                .collect()
        })
        .collect()
}

#[test]
fn native_sm53_binary_operations_keep_rounding_and_signed_zero() {
    for lanes in [1, 2, 4] {
        let f = vec(float(16), lanes);
        let all = cases(lanes);
        let pair: Vec<Case> = all.into_iter().map(|c| c[..2].to_vec()).collect();
        for op in [AirBin::FAdd, AirBin::FSub, AirBin::FMul] {
            let body = |fb: &mut FunctionBuilder<'_>, xs: &[Value]| {
                ops::sm53_binary(fb, op, lanes, xs[0], xs[1])
            };
            let m = kernel(&[f, f], f, &body).unwrap();
            verify_native(&m).unwrap();
            let native_ops: Vec<_> = m
                .blocks()
                .iter()
                .flat_map(|b| &b.insts)
                .filter(|i| {
                    matches!(
                        i.inst,
                        Inst::Binary {
                            op: BinOp::FAdd | BinOp::FSub | BinOp::FMul,
                            contract: Contraction::Forbidden,
                            ..
                        }
                    )
                })
                .collect();
            assert!(!native_ops.is_empty());
            check(
                "native SM53 binary",
                &[f, f],
                f,
                &body,
                &pair,
                &|c| {
                    Some(
                        c[0].iter()
                            .zip(&c[1])
                            .map(|(&x, &y)| {
                                let (x, y) = (f16_to_f64(x as u16), f16_to_f64(y as u16));
                                u64::from(f64_to_f16(match op {
                                    AirBin::FAdd => x + y,
                                    AirBin::FSub => x - y,
                                    _ => x * y,
                                }))
                            })
                            .collect(),
                    )
                },
                false,
            );
        }
    }
}

#[test]
fn native_sm53_fma_rounds_once_while_multiply_add_rounds_twice() {
    use crate::lower::arith::fma::{Fma, Route};
    for lanes in [1, 2, 4] {
        let f = vec(float(16), lanes);
        let all = cases(lanes);
        let fused = |fb: &mut FunctionBuilder<'_>, xs: &[Value]| {
            fb.module().set_native_half_conversions(true);
            crate::lower::arith::fma::emit(
                fb,
                Fma {
                    form: f,
                    route: Route::F16,
                },
                xs,
                None,
            )
        };
        let separate = |fb: &mut FunctionBuilder<'_>, xs: &[Value]| {
            let p = ops::sm53_binary(fb, AirBin::FMul, lanes, xs[0], xs[1])?;
            ops::sm53_binary(fb, AirBin::FAdd, lanes, p, xs[2])
        };
        let fused_module = kernel(&[f, f, f], f, &fused).unwrap();
        verify_native(&fused_module).unwrap();
        assert!(fused_module
            .blocks()
            .iter()
            .flat_map(|b| &b.insts)
            .any(|n| matches!(
                n.inst,
                Inst::Glsl {
                    op: GlslOp::Fma,
                    contract: Contraction::Forbidden,
                    ..
                }
            )));
        let separate_module = kernel(&[f, f, f], f, &separate).unwrap();
        verify_native(&separate_module).unwrap();
        assert!(!separate_module
            .blocks()
            .iter()
            .flat_map(|b| &b.insts)
            .any(|n| matches!(
                n.inst,
                Inst::Glsl {
                    op: GlslOp::Fma,
                    ..
                }
            )));
        let reference = |c: &Case, fused: bool| {
            Some(
                c[0].iter()
                    .zip(&c[1])
                    .zip(&c[2])
                    .map(|((&x, &y), &z)| {
                        let (x, y, z) = (
                            f16_to_f64(x as u16),
                            f16_to_f64(y as u16),
                            f16_to_f64(z as u16),
                        );
                        let answer = if fused {
                            x.mul_add(y, z)
                        } else {
                            f16_to_f64(f64_to_f16(x * y)) + z
                        };
                        u64::from(f64_to_f16(answer))
                    })
                    .collect(),
            )
        };
        assert!(all
            .iter()
            .any(|c| reference(c, true) != reference(c, false)));
        check(
            "native SM53 fma",
            &[f, f, f],
            f,
            &fused,
            &all,
            &|c| reference(c, true),
            false,
        );
        check(
            "native SM53 separate",
            &[f, f, f],
            f,
            &separate,
            &all,
            &|c| reference(c, false),
            false,
        );
    }
}

#[test]
fn native_sm53_rejects_half_division_and_contractible_operations() {
    let f = float(32);
    for op in [BinOp::FDiv, BinOp::FAdd] {
        let m = kernel(&[f, f], f, &|fb, xs| {
            let half = fb.module().ty(Type::Float { width: 16 })?;
            let x = fb.convert(ConvOp::FConvert, xs[0], half)?;
            let y = fb.convert(ConvOp::FConvert, xs[1], half)?;
            let v = fb.binary(op, x, y)?;
            let wide = fb.module().ty(Type::Float { width: 32 })?;
            fb.convert(ConvOp::FConvert, v, wide)
        })
        .unwrap();
        assert!(matches!(
            verify_native(&m),
            Err(VerifyError::Rule(Violation::NativeHalfOperation(_)))
        ));
    }
    assert!(!profile()
        .fields()
        .enabled
        .features
        .contains(&VkFeature::ShaderFloat16));
    let weak_fma = kernel(&[f, f, f], f, &|fb, xs| {
        let half = fb.module().ty(Type::Float { width: 16 })?;
        let values = xs
            .iter()
            .map(|&x| fb.convert(ConvOp::FConvert, x, half))
            .collect::<BResult<Vec<_>>>()?;
        let value = fb.glsl(GlslOp::Fma, &values)?;
        let wide = fb.module().ty(Type::Float { width: 32 })?;
        fb.convert(ConvOp::FConvert, value, wide)
    })
    .unwrap();
    assert!(matches!(
        verify_native(&weak_fma),
        Err(VerifyError::Rule(Violation::NativeHalfOperation(_)))
    ));
}

#[test]
fn native_sm53_conversion_covers_ties_subnormals_overflow_and_nans() {
    use crate::spirv::builder::QuantizeForm;
    let mut bits = vec![
        0, 0x80000000, 1, 0x80000001, 0x7f800000, 0xff800000, 0x7f800001, 0x7fc12345, 0xffffffff,
        0x7f7fffff, 0xff7fffff,
    ];
    for h in (0u16..0x7c00).step_by(13) {
        let low = f16_to_f64(h) as f32;
        let high = f16_to_f64(h + 1) as f32;
        let mid = (low + high) * 0.5;
        for at in [low.to_bits(), mid.to_bits()] {
            for delta in [-1i64, 0, 1] {
                let value = (i64::from(at) + delta).max(0) as u32;
                bits.extend([value, value | 0x80000000]);
            }
        }
    }
    for at in [
        0x33000000u32,
        0x33800000,
        0x387fe000,
        0x38800000,
        0x477fe000,
        0x477ff000,
    ] {
        for delta in [-1i64, 0, 1] {
            let value = (i64::from(at) + delta) as u32;
            bits.extend([value, value | 0x80000000]);
        }
    }
    let cases: Vec<Case> = bits.into_iter().map(|b| vec![vec![u64::from(b)]]).collect();
    check(
        "native SM53 conversion",
        &[float(32)],
        float(16),
        &|fb, xs| {
            fb.module().set_native_half_conversions(true);
            fb.module().set_quantize_form(QuantizeForm::Converted);
            super::super::narrow::round(fb, float(16), xs[0])
        },
        &cases,
        &|c| {
            Some(vec![u64::from(f64_to_f16(f64::from(f32::from_bits(
                c[0][0] as u32,
            ))))])
        },
        false,
    );
}

#[test]
fn native_sm53_conversion_serializes_explicit_nearest_even() {
    use crate::spirv::builder::QuantizeForm;
    for native in [false, true] {
        let module = kernel(&[float(32)], float(32), &|fb, xs| {
            fb.module().set_native_half_conversions(native);
            fb.module().set_quantize_form(QuantizeForm::Converted);
            super::super::narrow::round(fb, float(16), xs[0])
        })
        .unwrap();
        let words = crate::spirv::serialize::serialize(module).unwrap().words;
        let mut at = 5;
        let mut rounding = 0;
        while at < words.len() {
            let count = (words[at] >> 16) as usize;
            if words[at] & 0xffff == crate::spv::Op::Decorate as u32
                && words[at + 2] == crate::spv::Decoration::FPRoundingMode as u32
            {
                assert_eq!(words[at + 3], crate::spv::FPRoundingMode::RTE as u32);
                rounding += 1;
            }
            at += count;
        }
        assert_eq!(rounding, usize::from(native));
    }
}

#[test]
fn native_sm53_memory_conversions_preserve_every_half_bit_pattern() {
    use super::super::narrow;
    for lanes in [1, 2, 4] {
        let word = vec(int(32), lanes);
        let half = vec(float(16), lanes);
        let input: Vec<Case> = (0..=u16::MAX)
            .collect::<Vec<_>>()
            .chunks(usize::from(lanes))
            .map(|v| vec![v.iter().map(|&h| u64::from(h) | 0xa5530000).collect()])
            .collect();
        check(
            "native SM53 half memory bit preservation",
            &[word],
            word,
            &|fb, xs| {
                fb.module().set_native_half_conversions(true);
                let value = narrow::from_bits(fb, half, xs[0])?;
                narrow::to_bits(fb, half, value)
            },
            &input,
            &|c| Some(c[0].iter().map(|&h| h & 0xffff).collect()),
            false,
        );
        check(
            "native SM53 half decode",
            &[word],
            vec(float(32), lanes),
            &|fb, xs| {
                fb.module().set_native_half_conversions(true);
                narrow::from_bits(fb, half, xs[0])
            },
            &input,
            &|c| {
                Some(
                    c[0].iter()
                        .map(|&h| u64::from(narrow::f32_of_half_bits(h as u32)))
                        .collect(),
                )
            },
            false,
        );
        let mut values: Vec<u32> = (0..=u16::MAX)
            .map(|h| narrow::f32_of_half_bits(u32::from(h)))
            .collect();
        for payload in [1, 0x1fff, 0x2000, 0x3fff, 0x400000, 0x7fffff] {
            values.extend([0x7f800000 | payload, 0xff800000 | payload]);
        }
        let input: Vec<Case> = values
            .chunks(usize::from(lanes))
            .map(|v| {
                vec![(0..usize::from(lanes))
                    .map(|i| u64::from(v[i % v.len()]))
                    .collect()]
            })
            .collect();
        check(
            "native SM53 half encode",
            &[word],
            half,
            &|fb, xs| {
                fb.module().set_native_half_conversions(true);
                let wide = ops::ty(fb, vec(float(32), lanes))?;
                fb.convert(ConvOp::Bitcast, xs[0], wide)
            },
            &input,
            &|c| {
                Some(
                    c[0].iter()
                        .map(|&b| u64::from(narrow::half_bits_of_f32(b as u32)))
                        .collect(),
                )
            },
            false,
        );
    }
}
