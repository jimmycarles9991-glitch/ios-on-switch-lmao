// SPDX-License-Identifier: MIT
//! The target hardware's answers to the device queries: iPad7,1's A10X under 21H461, as `-[_MTLDevice initLimits]` fills
//! its limits table at the A10X's `featureProfile` (4) and as `AGXA10FamilyDevice` (AGXMetalA10) answers itself. The OS
//! on this device was built and tuned against these answers, so the device gives the target's answer wherever it
//! honours it and its own lower one where it does not, never more: an answer above the target's sends the OS down paths
//! no iPad7,1 takes. `gpuCoreCount` is not here: work sizing reads it as this GPU's real core count.

use metal2vulkan_runtime::Device;

/// The target's counts. The device answers the lower of its own and the target's; a count the runtime has no answer for
/// takes the target's, bounded by what the device takes (`device_bound`).
const COUNTS: &[(&str, u64)] = &[
    ("argumentBuffersSupport", 0),
    ("maxThreadgroupMemoryLength", 16384),
    ("maxComputeThreadgroupMemory", 16384),
    ("maxVertexTextures", 31),
    ("maxFragmentTextures", 31),
    ("maxComputeTextures", 31),
    ("maxTileBuffers", 0),
    ("maxTileTextures", 0),
    ("maxTileSamplers", 0),
    ("maxViewportCount", 1),
    ("maxFramebufferStorageBits", 256),
    ("maxVertexAttributes", 31),
    ("maxVertexInlineDataSize", 4096),
    ("maxFragmentInlineDataSize", 4096),
    ("maxComputeInlineDataSize", 4096),
    ("maxInterpolants", 60),
    ("maxInterpolatedComponents", 60),
    ("maxVisibilityQueryOffset", 65528),
    ("maxTextureWidth1D", 16384),
    ("maxTextureWidth3D", 2048),
    ("maxTextureHeight3D", 2048),
    ("maxTextureDepth3D", 2048),
    ("maxTextureDimensionCube", 16384),
    ("maxTextureBufferWidth", 1 << 28),
    ("maxFunctionConstantIndices", 65536),
    ("maxIndirectBuffers", 31),
    ("maxIndirectTextures", 31),
    ("maxIndirectSamplers", 16),
    ("maxIndirectSamplersPerDevice", 96),
    ("maxFenceInstances", 32768),
    ("maxCustomSamplePositions", 4),
    ("maxVertexAmplificationFactor", 1),
    ("maxVertexAmplificationCount", 1),
    ("maxComputeAttributes", 31),
    ("maxConstantBufferArguments", 31),
];

/// The target's flags that are no where the runtime's are yes.
const NO: &[&str] = &[
    "areRasterOrderGroupsSupported",
    "supportsRasterOrderGroups",
    "supportsSIMDGroup",
    "supportsSIMDShuffleAndFill",
    "supportsSIMDReduction",
    "supports32BitFloatFiltering",
    "supportsTexture2DMultisampleArray",
    "isLargeMRTSupported",
    "supportsComputeCompressedTextureWrite",
    "supportsConditionalLoadStore",
];

pub(crate) fn count(selector: &str) -> Option<u64> {
    COUNTS.iter().find(|(s, _)| *s == selector).map(|&(_, value)| value)
}

pub(crate) fn says_no(selector: &str) -> bool {
    NO.contains(&selector)
}

/// The target's counts the device answers, for the selectors the bridge installs.
pub(crate) fn count_selectors() -> impl Iterator<Item = &'static str> {
    COUNTS.iter().map(|&(s, _)| s)
}

/// What the device takes of a count the runtime has no answer for: its Vulkan limit where one bounds it, `u64::MAX`
/// where the runtime takes any value the target answers (its inline bytes are Metal's 4096, its argument tables and
/// fences have no limit of their own), `None` where it has none of it.
pub(crate) fn device_bound(device: &Device, selector: &str, color_attachments: u64) -> Option<u64> {
    let p = device.properties();
    let l = &p.description.limits;
    Some(match selector {
        "maxComputeThreadgroupMemory" => p.max_threadgroup_memory_length,
        "maxVertexAttributes" => l.max_vertex_input_attributes.into(),
        "maxInterpolants" | "maxInterpolatedComponents" => l.max_fragment_input_components.into(),
        "maxTextureWidth1D" => l.max_image_dimension_1d.into(),
        "maxTextureWidth3D" | "maxTextureHeight3D" | "maxTextureDepth3D" => l.max_image_dimension_3d.into(),
        "maxTextureDimensionCube" => l.max_image_dimension_cube.into(),
        "maxTextureBufferWidth" => l.max_texel_buffer_elements.into(),
        // The bits of a pixel across its color attachments: an immediate-mode GPU keeps no tile, so the widest format
        // (four 32-bit components) in every attachment.
        "maxFramebufferStorageBits" => color_attachments * 128,
        "maxCustomSamplePositions" => match p.are_programmable_sample_positions_supported() {
            Ok(true) => u64::MAX,
            _ => 0,
        },
        "maxVertexAmplificationFactor" | "maxVertexAmplificationCount" => {
            match p.supports_vertex_amplification_count(1) {
                Ok(true) => u64::MAX,
                _ => 0,
            }
        }
        "maxVertexInlineDataSize"
        | "maxFragmentInlineDataSize"
        | "maxComputeInlineDataSize"
        | "maxVisibilityQueryOffset"
        | "maxFunctionConstantIndices"
        | "maxIndirectBuffers"
        | "maxIndirectTextures"
        | "maxIndirectSamplers"
        | "maxIndirectSamplersPerDevice"
        | "maxFenceInstances"
        | "maxComputeAttributes"
        | "maxConstantBufferArguments" => u64::MAX,
        _ => return None,
    })
}
