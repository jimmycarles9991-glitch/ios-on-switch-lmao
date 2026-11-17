//! `powr`'s special operands on the interpreter, densely: every `half` base against every special
//! exponent and every special base against every `half` exponent, and every `f32` exponent field
//! (its least, middle and largest significands, both signs) against the specials, for
//! `fast::powr`'s rule (`math::fast_powr_special`), the precise `powr`'s NaN (`math::powr_nan`)
//! and `fast::pow`'s infinite product (`math::infinite_product`), each against the rule read from
//! the operands' bits, bit for bit, over a composition `r` the rules keep elsewhere.

use super::*;

const HALF_SPECIALS: [u64; 22] = [
    0x0000, 0x8000, 0x0001, 0x8001, 0x03FF, 0x83FF, 0x0400, 0x3800, 0xB800, 0x3BFF, 0x3C00, 0x3C01,
    0xBC00, 0x4000, 0x7BFF, 0xFBFF, 0x7C00, 0xFC00, 0x7C01, 0x7E00, 0xFE00, 0x7FFF,
];

const F32_SPECIALS: [u64; 24] = [
    0x0000_0000,
    0x8000_0000,
    0x0000_0001,
    0x8000_0001,
    0x007F_FFFF,
    0x807F_FFFF,
    0x0080_0000,
    0x8080_0000,
    0x3F00_0000,
    0xBF00_0000,
    0x3F7F_FFFF,
    0x3F80_0000,
    0x3F80_0001,
    0xBF80_0000,
    0x4000_0000,
    0x7F7F_FFFF,
    0xFF7F_FFFF,
    0x7F80_0000,
    0xFF80_0000,
    0x7F80_0001,
    0x7FC0_0000,
    0xFFC0_0000,
    0x7FFF_FFFF,
    0xFFFF_FFFF,
];

/// One format's encodings, read as `powr`'s rules read them.
#[derive(Clone, Copy)]
struct Width {
    sign: u64,
    inf: u64,
    one: u64,
    nan: u64,
    /// The magnitude below which Metal reads a value as a zero: an `f32`'s exponent field zero, a
    /// `half`'s zero alone (held widened, a `half` denormal is a normal `f32`).
    zero_below: u64,
}

const HALF: Width = Width {
    sign: 0x8000,
    inf: 0x7C00,
    one: 0x3C00,
    nan: 0x7E00,
    zero_below: 1,
};

const F32: Width = Width {
    sign: 0x8000_0000,
    inf: 0x7F80_0000,
    one: 0x3F80_0000,
    nan: 0x7FC0_0000,
    zero_below: 0x0080_0000,
};

impl Width {
    fn magnitude(self, b: u64) -> u64 {
        b & (self.sign - 1)
    }
    fn is_nan(self, b: u64) -> bool {
        self.magnitude(b) > self.inf
    }
    fn is_inf(self, b: u64) -> bool {
        self.magnitude(b) == self.inf
    }
    fn is_zero(self, b: u64) -> bool {
        self.magnitude(b) < self.zero_below
    }
    fn negative(self, b: u64) -> bool {
        b & self.sign != 0
    }
    /// Below zero as `fast::powr` compares: a negative number Metal does not read as a zero.
    fn below_fast(self, b: u64) -> bool {
        self.negative(b) && !self.is_zero(b) && !self.is_nan(b)
    }
    /// Below zero as the precise `powr` reads the sign: any nonzero encoding with it, a NaN's too.
    fn below_precise(self, b: u64) -> bool {
        self.negative(b) && self.magnitude(b) != 0
    }
    fn nan_rule(self, below: bool, x: u64, y: u64) -> bool {
        below
            || (self.is_zero(y) && (self.is_zero(x) || self.is_inf(x)))
            || (self.is_inf(y) && x == self.one)
    }
    fn numbers(self, x: u64, y: u64) -> bool {
        !self.is_nan(x) && !self.is_nan(y)
    }
    fn infinite_answer(self, x: u64, y: u64) -> u64 {
        if (self.magnitude(x) < self.one) != self.negative(y) {
            0
        } else {
            self.inf
        }
    }
    fn fast_powr(self, x: u64, y: u64, r: u64) -> u64 {
        if self.nan_rule(self.below_fast(x), x, y) {
            return self.nan;
        }
        let infinite = self.is_zero(x) || self.is_inf(x) || self.is_inf(y);
        if infinite && self.numbers(x, y) {
            return self.infinite_answer(x, y);
        }
        r
    }
    fn powr(self, x: u64, y: u64, r: u64) -> u64 {
        match self.nan_rule(self.below_precise(x), x, y) {
            true => self.nan,
            false => r,
        }
    }
    fn product(self, x: u64, y: u64, r: u64) -> u64 {
        let by_base = (self.is_zero(x) || self.is_inf(x)) && !self.is_zero(y);
        let by_exponent = self.is_inf(y) && x != self.one;
        match (by_base || by_exponent) && self.numbers(x, y) {
            true => self.infinite_answer(x, y),
            false => r,
        }
    }
}

/// Every `f32` exponent field with its least, middle and largest significands, of both signs.
fn f32_dense() -> Vec<u64> {
    let mut v = Vec::new();
    for e in 0..=0xFFu64 {
        for m in [0, 1, 0x3F_FFFF, 0x40_0000, 0x7F_FFFF] {
            for s in [0, 0x8000_0000] {
                v.push(s | (e << 23) | m);
            }
        }
    }
    v
}

/// The cases of part `part` of `of`: each operand of `all` against each special, either way round,
/// with the composition `r` third.
fn cases(all: &[u64], specials: &[u64], r: u64, part: usize, of: usize) -> Vec<Case> {
    let mut pairs = Vec::new();
    for &a in all {
        for &s in specials {
            pairs.push((a, s));
            pairs.push((s, a));
        }
    }
    part_of(pairs, part, of)
        .into_iter()
        .map(|(x, y)| vec![vec![x], vec![y], vec![r]])
        .collect()
}

type Rule = fn(Width, u64, u64, u64) -> u64;

fn rules() -> [(&'static str, Rule); 3] {
    [
        ("fast::powr", Width::fast_powr),
        ("powr", Width::powr),
        ("infinite product", Width::product),
    ]
}

fn body(name: &str, f: Form) -> impl Fn(&mut FunctionBuilder<'_>, &[Value]) -> BResult<Value> {
    let name = name.to_owned();
    move |fb, x| match name.as_str() {
        "fast::powr" => math::fast_powr_special(fb, f, x[0], x[1], x[2]),
        "powr" => math::powr_nan(fb, f, x[0], x[1], x[2]),
        _ => math::infinite_product(fb, f, x[1], x[0], x[2]),
    }
}

/// Each rule's module, verified for the native GM20B target the raw path compiles for, run on the
/// interpreter over `cases` and held to the rule bit for bit. The modules are not validated against
/// the committed Vulkan profiles (`run_module`), which this tree imports without.
fn check_grid(
    width: Width,
    f: Form,
    all: &[u64],
    specials: &[u64],
    r: u64,
    part: usize,
    of: usize,
) {
    let cases = cases(all, specials, r, part, of);
    let ins = [f, f, f];
    for (name, rule) in rules() {
        let what = format!("{name} {f:?} part {part}");
        let m = kernel_with(&ins, f, &body(name, f), true, false).expect("build");
        super::native_sm53::verify_native(&m).unwrap_or_else(|e| panic!("{what}: {e:?}"));
        let mut failures = Vec::new();
        for chunk in cases.chunks(CASES_PER_DISPATCH) {
            for (c, got) in chunk.iter().zip(dispatch_module(&what, &m, &ins, f, chunk)) {
                let want = rule(width, arg(c, 0), arg(c, 1), arg(c, 2));
                if got != [want] {
                    failures.push(format!("{c:x?}: got {got:x?}, want {want:x}"));
                }
            }
        }
        assert!(
            failures.is_empty(),
            "{what}: {} of {} cases differ:\n{}",
            failures.len(),
            cases.len(),
            failures[..failures.len().min(12)].join("\n")
        );
    }
}

fn half_grid(part: usize, of: usize) {
    let all: Vec<u64> = (0..=0xFFFF).collect();
    check_grid(HALF, float(16), &all, &HALF_SPECIALS, 0x3D00, part, of);
}

fn f32_grid(part: usize, of: usize) {
    let r = u64::from(1.25f32.to_bits());
    check_grid(F32, float(32), &f32_dense(), &F32_SPECIALS, r, part, of);
}

parts!(half_grid, 8;
    powr_half_specials_0 = 0, powr_half_specials_1 = 1, powr_half_specials_2 = 2,
    powr_half_specials_3 = 3, powr_half_specials_4 = 4, powr_half_specials_5 = 5,
    powr_half_specials_6 = 6, powr_half_specials_7 = 7,
);

parts!(f32_grid, 2; powr_f32_specials_0 = 0, powr_f32_specials_1 = 1);

/// The 32-bit integer constants of module `m`'s words.
fn integer_constants(m: TypedModule) -> Vec<u32> {
    const OP_TYPE_INT: u32 = 21;
    const OP_CONSTANT: u32 = 43;
    let words = crate::spirv::serialize::serialize(m).expect("words").words;
    let (mut ints, mut values) = (Vec::new(), Vec::new());
    let mut i = 5;
    while i < words.len() {
        let (n, code) = ((words[i] >> 16) as usize, words[i] & 0xFFFF);
        if code == OP_TYPE_INT && words.get(i + 2) == Some(&32) {
            ints.push(words[i + 1]);
        }
        if code == OP_CONSTANT && n == 4 && ints.contains(&words[i + 1]) {
            values.push(words[i + 3]);
        }
        i += n.max(1);
    }
    values
}

/// The rules compare with one as a float, which the native target makes against an immediate:
/// none of the three modules holds one's bits as an integer constant, on `half` or on `float`.
#[test]
fn powr_rules_compare_one_as_a_float() {
    for f in [float(16), float(32)] {
        for (name, _) in rules() {
            let m = kernel_with(&[f, f, f], f, &body(name, f), true, false).expect("build");
            let ints = integer_constants(m);
            assert!(!ints.contains(&0x3F80_0000), "{name} {f:?}: {ints:x?}");
        }
    }
}
