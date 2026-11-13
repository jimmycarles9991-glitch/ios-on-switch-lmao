#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub enum NativeShaderTarget {
    #[default]
    Vulkan,
    Sm53,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HalfOperation {
    Convert,
    Add,
    Subtract,
    Multiply,
    FusedMultiplyAdd,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HalfInstruction {
    Convert,
    Add2,
    Multiply2,
    FusedMultiplyAdd2,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct HalfRules {
    pub instruction: HalfInstruction,
    pub nearest_even: bool,
    pub preserve_denormals: bool,
    pub overflow_to_infinity: bool,
    pub nan_payload_preserved: bool,
    pub separate_rounding: bool,
}

impl NativeShaderTarget {
    pub(crate) fn property(self, property: super::requirements::VkProperty) -> bool {
        use super::requirements::{FloatControl, FloatWidth, VkProperty};
        self == Self::Sm53
            && matches!(
                property,
                VkProperty::NativeSm53Half
                    | VkProperty::FloatControl(
                        FloatControl::SignedZeroInfNanPreserve,
                        FloatWidth::F16
                    )
            )
    }

    /// Whether the device's color store makes Metal's code of an `n`-bit unorm channel from the
    /// value `color_write::Lane::StoreCode` writes. GM20B's ROP truncates the value to `n + 4`
    /// fractional bits and rounds the product with the channel's maximum to nearest, ties down
    /// (254,892 of 254,892 device readings of 8- and 10-bit channels and RGB10A2's alpha, the
    /// top two bits of its 10-bit code; the rule matched an independent 16-bit sweep), so a code's
    /// `f32` nearest value stores that code.
    pub(crate) fn stores_unorm_code(self, bits: u8) -> bool {
        self == Self::Sm53 && matches!(bits, 2 | 8 | 10 | 16)
    }

    /// Whether the device's color store makes Metal's half of a `float` output to a half-float
    /// channel rounding toward zero: GM20B's ROP converts to a half toward zero, a finite value
    /// past the largest to the largest, an infinity kept, -0 kept, and stores the NaN it is given
    /// as `0x7e00`, Metal's NaN of a `float` output (54,467 of 54,468 device readings of every
    /// half in [0, 1], the values beside them and the specials; the other one the NaN).
    pub(crate) fn stores_half_toward_zero(self) -> bool {
        self == Self::Sm53
    }

    pub(crate) fn half(self, operation: HalfOperation) -> bool {
        self.half_rules(operation).is_some()
    }

    pub(crate) fn half_rules(self, operation: HalfOperation) -> Option<HalfRules> {
        match self {
            Self::Vulkan => None,
            Self::Sm53 => Some(HalfRules {
                instruction: match operation {
                    HalfOperation::Convert => HalfInstruction::Convert,
                    HalfOperation::Add | HalfOperation::Subtract => HalfInstruction::Add2,
                    HalfOperation::Multiply => HalfInstruction::Multiply2,
                    HalfOperation::FusedMultiplyAdd => HalfInstruction::FusedMultiplyAdd2,
                },
                nearest_even: true,
                preserve_denormals: true,
                overflow_to_infinity: true,
                nan_payload_preserved: false,
                separate_rounding: operation != HalfOperation::FusedMultiplyAdd,
            }),
        }
    }
}
