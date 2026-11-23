//! Metal's 16-bit float blend of a `half4` output in a module without fast math
//! (`air.compile.fast_math_disable`), which the function computes in the shader, inside the pixel
//! interlock, from the texel it reads, where the profile asks for it (`DevicePolicy::half_blend`):
//! Metal rounds each step of that blend to `half`, where the device rounds it once.
//!
//! **The rule** (EV FLT-hb2-m3; the same forms Metal's compiler takes for a blend it folds into a
//! function without fast math, EVs XRB-sw1s-m4 and XRB-sw2s-m4). The color lanes the write stores
//! (two or three of them, one vector) and alpha (a scalar) each compute `S op D`, `S = s Fs` and
//! `D = d Fd`: each term's product rounded to `half`, nearest even (a factor of one its operand, of
//! zero no term), and the two terms' sum rounded ([`Arith::Rounded`]); `Min` and `Max` take
//! `min(s, d)` and `max(s, d)`, the factors unread. `SourceAlphaSaturated` is `min(sa, h(1 - da))`
//! in color and one in alpha, and every `1 - x` is `h(1 - x)` of the `half` it reads, the blend
//! color's too.
//!
//! **Not built** ([`ExtendedRangeBlend`]): a factor of the second source, whose output is not
//! planned beside the storage form's write.

use super::floats;
use crate::error::{Error, ExtendedRangeBlend, Feature, Unsupported};
use crate::lower::arith::fma::half_sum;
use crate::lower::arith::legal::{FloatFmt, Form, Held};
use crate::lower::arith::ops::{fsplat, ty};
use crate::lower::arith::texel_round::{f32_to_half, HalfRounding};
use crate::metal::{BlendFactor, BlendOperation, ColorWriteMask};
use crate::pipeline::lower_inputs::BlendEquation;
use crate::spirv::builder::FunctionBuilder;
use crate::spirv::error::{BResult, BuildError, TypeRule};
use crate::spirv::ids::Value;
use crate::spirv::ir::{BinOp, ConvOp, GlslOp, UnOp};

/// A value a blend lane reads.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Atom {
    /// The source's lane: a color lane's own channel, alpha's alpha.
    S,
    /// The source's alpha.
    Sa,
    /// The destination's lane (the texel's).
    D,
    /// The destination's alpha.
    Da,
    /// The blend color's lane.
    C,
    /// The blend color's alpha.
    Ca,
}

/// A factor of a blend lane.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Node {
    Zero,
    One,
    Atom(Atom),
    /// `h(1 - x)`.
    OneMinus(Atom),
    /// `min(sa, h(1 - da))`, a color lane's `SourceAlphaSaturated`.
    Saturate,
}

/// An operand and its factor: their product.
pub(crate) type Term = (Atom, Node);

/// The arithmetic of one lane class, in the module docs' forms.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum Arith {
    /// `min(s, d)` or `max(s, d)`, `s` the `source` atom.
    MinMax {
        max: bool,
        source: Atom,
    },
    Zero,
    /// `h(sign · term)`.
    Term {
        sign: i8,
        term: Term,
    },
    /// `h(h(sign · source) + h(sign · destination))`, a term of factor one its operand.
    Rounded {
        source: (i8, Term),
        destination: (i8, Term),
    },
}

/// Metal's 16-bit float blend of one attachment's write, as the shader computes it
/// ([`blend_half_float`]).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Blend {
    /// The color lanes' arithmetic, where the write stores two or three of them.
    pub(crate) color: Option<Arith>,
    /// Alpha's, where the write stores it.
    pub(crate) alpha: Option<Arith>,
}

/// The function's math mode, which decides whether Metal's 16-bit float blend of its `half4`
/// output is the shader's ([`Blend::half_float`], measured without fast math alone).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BlendMath {
    /// Fast math, or a module that names no mode.
    Fast,
    /// A module without fast math (`air.compile.fast_math_disable`): each term's product rounded
    /// to `half`, then the terms' sum ([`Arith::Rounded`]).
    Rounded,
}

impl BlendMath {
    /// The blend math of a module compiled with `fast_math` (`CompileOptions::fast_math`).
    pub fn of(fast_math: Option<bool>) -> BlendMath {
        match fast_math {
            Some(false) => BlendMath::Rounded,
            Some(true) | None => BlendMath::Fast,
        }
    }
}

fn refused(gap: ExtendedRangeBlend) -> Error {
    Unsupported::not_implemented(Feature::ExtendedRangeBlend(gap)).into()
}

fn placeholder(ty: &'static str, value: u32) -> Error {
    Error::InvalidDescriptor(crate::error::DescriptorError::PlaceholderEnumValue { ty, value })
}

/// Factor `f` of a color lane, or of alpha where `alpha`: a factor of the second source is
/// refused ([`ExtendedRangeBlend::DualSource`]).
fn factor(f: BlendFactor, alpha: bool) -> Result<Node, Error> {
    use BlendFactor as F;
    let own = |color: Atom, a: Atom| if alpha { a } else { color };
    Ok(match f {
        F::Zero => Node::Zero,
        F::One => Node::One,
        F::SourceColor => Node::Atom(own(Atom::S, Atom::Sa)),
        F::OneMinusSourceColor => Node::OneMinus(own(Atom::S, Atom::Sa)),
        F::SourceAlpha => Node::Atom(Atom::Sa),
        F::OneMinusSourceAlpha => Node::OneMinus(Atom::Sa),
        F::DestinationColor => Node::Atom(own(Atom::D, Atom::Da)),
        F::OneMinusDestinationColor => Node::OneMinus(own(Atom::D, Atom::Da)),
        F::DestinationAlpha => Node::Atom(Atom::Da),
        F::OneMinusDestinationAlpha => Node::OneMinus(Atom::Da),
        F::SourceAlphaSaturated if alpha => Node::One,
        F::SourceAlphaSaturated => Node::Saturate,
        F::BlendColor => Node::Atom(own(Atom::C, Atom::Ca)),
        F::OneMinusBlendColor => Node::OneMinus(own(Atom::C, Atom::Ca)),
        F::BlendAlpha => Node::Atom(Atom::Ca),
        F::OneMinusBlendAlpha => Node::OneMinus(Atom::Ca),
        F::Source1Color | F::OneMinusSource1Color | F::Source1Alpha | F::OneMinusSource1Alpha => {
            return Err(refused(ExtendedRangeBlend::DualSource))
        }
        F::Unspecialized => return Err(placeholder(BlendFactor::METAL_NAME, f.as_raw())),
    })
}

/// The arithmetic of a lane class (alpha where `alpha`) of factors `fs` and `fd` under `op`:
/// `min` or `max`; no term, one, or both terms' rounded products added and rounded
/// ([`Arith::Rounded`]).
fn plan(
    (fs, fd, op): (BlendFactor, BlendFactor, BlendOperation),
    alpha: bool,
) -> Result<Arith, Error> {
    let (s, d) = match alpha {
        true => (Atom::Sa, Atom::Da),
        false => (Atom::S, Atom::D),
    };
    let (ps, pd): (i8, i8) = match op {
        BlendOperation::Add => (1, 1),
        BlendOperation::Subtract => (1, -1),
        BlendOperation::ReverseSubtract => (-1, 1),
        BlendOperation::Min | BlendOperation::Max => {
            return Ok(Arith::MinMax {
                max: op == BlendOperation::Max,
                source: s,
            })
        }
        BlendOperation::Unspecialized => {
            return Err(placeholder(BlendOperation::METAL_NAME, op.as_raw()))
        }
    };
    let (nfs, nfd) = (factor(fs, alpha)?, factor(fd, alpha)?);
    let (st, dt): (Term, Term) = ((s, nfs), (d, nfd));
    Ok(match (nfs == Node::Zero, nfd == Node::Zero) {
        (true, true) => Arith::Zero,
        (true, false) => Arith::Term { sign: pd, term: dt },
        (false, true) => Arith::Term { sign: ps, term: st },
        (false, false) => Arith::Rounded {
            source: (ps, st),
            destination: (pd, dt),
        },
    })
}

impl Blend {
    /// Metal's blend `eq` of a `half4` output of a module without fast math into a 16-bit float
    /// attachment (`R16Float`, `RG16Float`, `RGBA16Float`) storing the channels `written` of four
    /// (a channel the format lacks counts as written: the store drops it), measured into those
    /// formats (EV FLT-hb2-m3: the output's `half`, each factor, each product and the sum rounded
    /// to `half`, nearest even; the blend color rounded to `half` before its one minus), computed by
    /// [`blend_half_float`]. A factor of the second source is no form here
    /// (`Feature::ExtendedRangeBlend` `DualSource`): its output is not planned beside the storage
    /// form's write.
    pub fn half_float(eq: &BlendEquation, written: ColorWriteMask) -> Result<Blend, Error> {
        let colors = [
            ColorWriteMask::RED,
            ColorWriteMask::GREEN,
            ColorWriteMask::BLUE,
        ]
        .into_iter()
        .any(|c| written.contains(c));
        let color = match colors {
            true => Some(plan(
                (eq.source_rgb, eq.destination_rgb, eq.rgb_operation),
                false,
            )?),
            false => None,
        };
        let alpha = match written.contains(ColorWriteMask::ALPHA) {
            true => Some(plan(
                (eq.source_alpha, eq.destination_alpha, eq.alpha_operation),
                true,
            )?),
            false => None,
        };
        Ok(Blend { color, alpha })
    }

    /// Whether the blend reads the blend color (`PushField::HalfBlendColor`).
    pub fn reads_constant(&self) -> bool {
        [&self.color, &self.alpha]
            .into_iter()
            .flatten()
            .any(|a| nodes(a).into_iter().any(reads_constant))
    }
}

/// Whether `n` reads the blend color.
fn reads_constant(n: Node) -> bool {
    matches!(
        n,
        Node::Atom(Atom::C | Atom::Ca) | Node::OneMinus(Atom::C | Atom::Ca)
    )
}

/// Every node `a` reads.
fn nodes(a: &Arith) -> Vec<Node> {
    let term = |t: &Term| [Node::Atom(t.0), t.1];
    match a {
        Arith::MinMax { .. } | Arith::Zero => Vec::new(),
        Arith::Term { term: t, .. } => term(t).to_vec(),
        Arith::Rounded {
            source,
            destination,
        } => [term(&source.1), term(&destination.1)].concat(),
    }
}

/// How a lane class's values are held: the color lanes in a `vec4` (lane 3 unread), alpha a
/// scalar.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Lanes {
    Vector,
    Scalar,
}

impl Lanes {
    fn floats(self) -> Form {
        Form {
            held: Held::Float(FloatFmt::F32),
            lanes: match self {
                Lanes::Vector => 4,
                Lanes::Scalar => 1,
            },
        }
    }
}

/// The `vec4`s a blend reads: the source, the texel, and the blend color rounded to `half` where
/// the blend reads it.
struct Inputs {
    s: Value,
    d: Value,
    constant: Option<Value>,
}

/// The values of one lane class, each node computed once.
struct Class<'a> {
    inputs: &'a Inputs,
    lanes: Lanes,
    nodes: Vec<(Node, Value)>,
}

impl<'a> Class<'a> {
    fn of(inputs: &'a Inputs, lanes: Lanes) -> Self {
        Class {
            inputs,
            lanes,
            nodes: Vec::new(),
        }
    }

    /// `v` (a `vec4`) as this class holds it: the vector, or its lane 3.
    fn own(&self, fb: &mut FunctionBuilder<'_>, v: Value) -> BResult<Value> {
        match self.lanes {
            Lanes::Vector => Ok(v),
            Lanes::Scalar => fb.composite_extract(v, &[3]),
        }
    }

    /// Lane 3 of `v` (a `vec4`), in every lane of this class.
    fn splat(&self, fb: &mut FunctionBuilder<'_>, v: Value) -> BResult<Value> {
        match self.lanes {
            Lanes::Vector => fb.vector_shuffle(v, v, &[3, 3, 3, 3]),
            Lanes::Scalar => fb.composite_extract(v, &[3]),
        }
    }

    fn constant(&self) -> BResult<Value> {
        self.inputs
            .constant
            .ok_or(BuildError::Type(TypeRule::FunctionSignature))
    }

    fn atom(&mut self, fb: &mut FunctionBuilder<'_>, a: Atom) -> BResult<Value> {
        let i = self.inputs;
        match a {
            Atom::S => self.own(fb, i.s),
            Atom::Sa => self.splat(fb, i.s),
            Atom::D => self.own(fb, i.d),
            Atom::Da => self.splat(fb, i.d),
            Atom::C => {
                let c = self.constant()?;
                self.own(fb, c)
            }
            Atom::Ca => {
                let c = self.constant()?;
                self.splat(fb, c)
            }
        }
    }

    /// `h(1 - x)` of `half` `x`: `1 - x` is exact in `f32` but for `|x|` below `2^-13`, where its
    /// rounding stays clear of a `half` midpoint, so it rounds once.
    fn one_minus(&mut self, fb: &mut FunctionBuilder<'_>, x: Value) -> BResult<Value> {
        let f = self.lanes.floats();
        let one = fsplat(fb, f, 1.0)?;
        let m = fb.binary(BinOp::FSub, one, x)?;
        f32_to_half(fb, f, m, HalfRounding::NearestEven)
    }

    fn node(&mut self, fb: &mut FunctionBuilder<'_>, n: Node) -> BResult<Value> {
        if let Some(&(_, v)) = self.nodes.iter().find(|(m, _)| *m == n) {
            return Ok(v);
        }
        let f = self.lanes.floats();
        let v = match n {
            Node::Zero => fsplat(fb, f, 0.0)?,
            Node::One => fsplat(fb, f, 1.0)?,
            Node::Atom(a) => self.atom(fb, a)?,
            Node::OneMinus(a) => {
                let x = self.atom(fb, a)?;
                self.one_minus(fb, x)?
            }
            Node::Saturate => {
                let sa = self.atom(fb, Atom::Sa)?;
                let da = self.atom(fb, Atom::Da)?;
                let m = self.one_minus(fb, da)?;
                fb.glsl(GlslOp::FMin, &[sa, m])?
            }
        };
        self.nodes.push((n, v));
        Ok(v)
    }

    /// `x`, negated where `sign` is negative.
    fn scaled(&self, fb: &mut FunctionBuilder<'_>, sign: i8, x: Value) -> BResult<Value> {
        match sign < 0 {
            true => fb.unary(UnOp::FNegate, x),
            false => Ok(x),
        }
    }

    fn round(&self, fb: &mut FunctionBuilder<'_>, x: Value) -> BResult<Value> {
        f32_to_half(fb, self.lanes.floats(), x, HalfRounding::NearestEven)
    }

    /// `sign · operand`, and the factor, of `(sign, term)`.
    fn signed(
        &mut self,
        fb: &mut FunctionBuilder<'_>,
        (sign, t): (i8, Term),
    ) -> BResult<[Value; 2]> {
        let x = self.atom(fb, t.0)?;
        let x = self.scaled(fb, sign, x)?;
        let f = self.node(fb, t.1)?;
        Ok([x, f])
    }

    fn eval(&mut self, fb: &mut FunctionBuilder<'_>, a: &Arith) -> BResult<Value> {
        match a {
            Arith::MinMax { max, source } => {
                let d = match self.lanes {
                    Lanes::Vector => Atom::D,
                    Lanes::Scalar => Atom::Da,
                };
                let s = self.atom(fb, *source)?;
                let d = self.atom(fb, d)?;
                let op = match max {
                    true => GlslOp::FMax,
                    false => GlslOp::FMin,
                };
                fb.glsl(op, &[s, d])
            }
            Arith::Zero => fsplat(fb, self.lanes.floats(), 0.0),
            Arith::Term { sign, term } => self.product(fb, (*sign, *term)),
            Arith::Rounded {
                source,
                destination,
            } => {
                let s = self.product(fb, *source)?;
                let d = self.product(fb, *destination)?;
                // `h(s + d)` of `half`s, exactly.
                let f = self.lanes.floats();
                let one = fsplat(fb, f, 1.0)?;
                let sum = half_sum(fb, f.with(Held::Float(FloatFmt::Half)), s, one, d)?;
                self.round(fb, sum)
            }
        }
    }

    /// `h(sign · operand · factor)` of `(sign, term)`: the operand alone where the factor is one
    /// (a `half`); a product of two `half`s is exact in `f32`.
    fn product(&mut self, fb: &mut FunctionBuilder<'_>, (sign, t): (i8, Term)) -> BResult<Value> {
        let [x, f] = self.signed(fb, (sign, t))?;
        match t.1 {
            Node::One => Ok(x),
            _ => {
                let p = fb.binary(BinOp::FMul, x, f)?;
                self.round(fb, p)
            }
        }
    }
}

/// Metal's blend `b` ([`Blend::half_float`]) of a `half4` output `s` (a `vec4` of `f32` holding
/// its `half`s) over `d`, the 16-bit float attachment's texel as the storage image reads it (a
/// `vec4` of `f32`; a channel the format lacks reads 0, alpha 1), in a module without fast math:
/// a `vec4` of `f32` holding each blended lane, a `half`. Nothing is clamped: the source is the
/// `half` the function returns, and `color`, the encoder's blend color (a `uvec4` of its `f32`s'
/// bits, `PushField::HalfBlendColor`, where `b` reads it, [`Blend::reads_constant`]), is rounded
/// to `half`, nearest even, which `BlendColor` reads and whose one minus `OneMinusBlendColor`
/// rounds again (EV FLT-hb2-m3: the one fit of `RGBA16Float`'s blue lane under a blend color of
/// 0.9). `min` and `max` are the device's `FMin` and `FMax`, whose answer at a NaN is not Metal's
/// measured one.
pub fn blend_half_float(
    fb: &mut FunctionBuilder<'_>,
    b: &Blend,
    (s, d): (Value, Value),
    color: Option<Value>,
) -> BResult<Value> {
    let constant = match color {
        Some(words) => {
            let ft = ty(fb, floats())?;
            let c = fb.convert(ConvOp::Bitcast, words, ft)?;
            Some(f32_to_half(fb, floats(), c, HalfRounding::NearestEven)?)
        }
        None => None,
    };
    let inputs = Inputs { s, d, constant };
    lanes(fb, b, &inputs)
}

/// The `vec4` of blend `b`'s lanes over `inputs`: the color class's three, and alpha's, each
/// lane the write does not store `d`'s.
fn lanes(fb: &mut FunctionBuilder<'_>, b: &Blend, inputs: &Inputs) -> BResult<Value> {
    let f = floats();
    let d = inputs.d;
    let color = match &b.color {
        Some(a) => Class::of(inputs, Lanes::Vector).eval(fb, a)?,
        None => d,
    };
    let alpha = match &b.alpha {
        Some(a) => Some(Class::of(inputs, Lanes::Scalar).eval(fb, a)?),
        None => None,
    };
    let ft = ty(fb, f)?;
    let mut lanes = Vec::with_capacity(4);
    for k in 0..3u32 {
        lanes.push(fb.composite_extract(color, &[k])?);
    }
    lanes.push(match alpha {
        Some(a) => a,
        None => fb.composite_extract(d, &[3])?,
    });
    fb.composite_construct(ft, &lanes)
}

#[cfg(test)]
mod tests;
