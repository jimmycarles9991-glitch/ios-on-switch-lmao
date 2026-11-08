// SPDX-License-Identifier: MIT
//! The target hardware's answers to the device queries: the device the OS of this build identifies the hardware as.
//! iOS 17 (21H461) runs as iPad7,1 (J120): its A10X as `-[_MTLDevice initLimits]` fills the limits table at the A10X's
//! `featureProfile` (4) and as `AGXA10FamilyDevice` (AGXMetalA10) answers itself. iOS 27 (24B119, `--cfg
//! target_j181`) runs as iPad12,1 (J181): its A13 at `featureProfile` 7 and as `AGXA13FamilyDevice` (AGXMetalA13)
//! answers itself. The OS on this device was built and tuned against these answers, so the device gives the target's
//! answer wherever it honours it and its own lower one where it does not, never more: an answer above the target's sends
//! the OS down paths the target never takes. `gpuCoreCount` is not here: work sizing reads it as this GPU's real core count.

use metal2vulkan_runtime::Device;

#[cfg(not(target_j181))]
use a10x as identity;
#[cfg(target_j181)]
use a13 as identity;

pub(crate) use identity::FAMILIES;

#[cfg_attr(target_j181, allow(dead_code))]
mod a10x {
    use metal2vulkan_runtime::GpuFamily;

    /// The target's counts. The device answers the lower of its own and the target's; a count the runtime has no
    /// answer for takes the target's, bounded by what the device takes (`device_bound`).
    pub(super) const COUNTS: &[(&str, u64)] = &[
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
    pub(super) const NO: &[&str] = &[
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

    /// The GPU families of the target hardware (MTLGPUFamilyApple3 and the families below it), which the target OS was
    /// built against and which `supportsFamily:` answers true even where rows of Apple's tables are not exact here. The
    /// OS takes its only working path through them: Core Image's Metal context needs Apple2 (`CIMetalDeviceIsSupported`),
    /// and without it falls back to an OpenGL ES context this device has none of (iconservicesagent crashed in
    /// `CI::GLContext::init`, int-155); QuartzCore splits a frame into command buffers of 128 draws where Apple3 is false
    /// (`MetalContext::flush`). Each waived row stays the runtime's own refusal where it is used, and
    /// `nmb_device_family_waivers` names them for the journal.
    pub(crate) const FAMILIES: &[GpuFamily] = &[
        GpuFamily::Apple1,
        GpuFamily::Apple2,
        GpuFamily::Apple3,
        GpuFamily::Common1,
        GpuFamily::Common2,
    ];
}

/// The A13 of 24B119 (featureProfile 7: `-[_MTLDevice initLimits]` and `AGXA13FamilyDevice`'s own answers), where this
/// GPU backs it. The tile counts stay 0: tile shaders and imageblocks have no GM20B form.
#[cfg_attr(not(target_j181), allow(dead_code))]
mod a13 {
    use metal2vulkan_runtime::GpuFamily;

    pub(super) const COUNTS: &[(&str, u64)] = &[
        ("argumentBuffersSupport", 1),
        ("maxThreadgroupMemoryLength", 32768),
        ("maxComputeThreadgroupMemory", 65536),
        ("maxVertexTextures", 128),
        ("maxFragmentTextures", 128),
        ("maxComputeTextures", 128),
        ("maxTileBuffers", 0),
        ("maxTileTextures", 0),
        ("maxTileSamplers", 0),
        ("maxViewportCount", 16),
        ("maxFramebufferStorageBits", 512),
        ("maxVertexAttributes", 31),
        ("maxVertexInlineDataSize", 4096),
        ("maxFragmentInlineDataSize", 4096),
        ("maxComputeInlineDataSize", 4096),
        ("maxInterpolants", 124),
        ("maxInterpolatedComponents", 124),
        ("maxVisibilityQueryOffset", 65528),
        ("maxTextureWidth1D", 16384),
        ("maxTextureWidth3D", 2048),
        ("maxTextureHeight3D", 2048),
        ("maxTextureDepth3D", 2048),
        ("maxTextureDimensionCube", 16384),
        ("maxTextureBufferWidth", 1 << 28),
        ("maxFunctionConstantIndices", 65536),
        ("maxIndirectBuffers", 500000),
        ("maxIndirectTextures", 500000),
        ("maxIndirectSamplers", 500000),
        ("maxIndirectSamplersPerDevice", 96),
        ("maxFenceInstances", 32768),
        ("maxCustomSamplePositions", 4),
        ("maxVertexAmplificationFactor", 8),
        ("maxVertexAmplificationCount", 2),
        ("maxComputeAttributes", 31),
        ("maxConstantBufferArguments", 31),
    ];

    pub(super) const NO: &[&str] = &[
        "supportsSIMDShuffleAndFill",
        "supportsSIMDReduction",
        "supports32BitFloatFiltering",
        "supportsTexture2DMultisampleArray",
        "isLargeMRTSupported",
        "supportsComputeCompressedTextureWrite",
        "supportsConditionalLoadStore",
    ];

    /// J181 answers Apple1 to Apple6 and Common1 to Common3. Apple4 and up promise imageblocks and tile shaders, which
    /// this GPU has no form of, and QuartzCore 27 takes Apple4 as exactly that promise: with it, every blur and backdrop
    /// capture is a compute kernel over imageblocks (`create_blur_surface_internal`'s BlurStateCompute,
    /// `capture_backdrop`'s " no_image_blocks" when it is false), without it its fragment path.
    pub(crate) const FAMILIES: &[GpuFamily] = &[
        GpuFamily::Apple1,
        GpuFamily::Apple2,
        GpuFamily::Apple3,
        GpuFamily::Common1,
        GpuFamily::Common2,
        GpuFamily::Common3,
    ];
}

pub(crate) fn count(selector: &str) -> Option<u64> {
    identity::COUNTS
        .iter()
        .find(|(s, _)| *s == selector)
        .map(|&(_, value)| value)
}

pub(crate) fn says_no(selector: &str) -> bool {
    identity::NO.contains(&selector)
}

/// The target's counts the device answers, for the selectors the bridge installs.
pub(crate) fn count_selectors() -> impl Iterator<Item = &'static str> {
    identity::COUNTS.iter().map(|&(s, _)| s)
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
        "maxVertexAmplificationFactor" | "maxVertexAmplificationCount" => (1..=8)
            .take_while(|&n| matches!(p.supports_vertex_amplification_count(n), Ok(true)))
            .last()
            .map_or(0, |n| if n == 8 { u64::MAX } else { n }),
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
