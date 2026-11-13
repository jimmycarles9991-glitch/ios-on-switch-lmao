// SPDX-License-Identifier: MIT
// Validation before emission: a planned command buffer is plannable, or the first thing wrong with it, classed by what
// Metal makes of it. Nothing after this refuses: lowering takes every plannable graph.
#pragma once
#include "plan_build.h"

namespace raw::plan {

enum class Verdict : uint8_t {
    Plannable,
    ClientError,  // Metal's validation layer rejects it and defines no NSError: the command is dropped, logged once
    QueryGated,   // the device answers that it lacks the feature: a client error the client was expected to avoid
    CommandBufferError,  // the command buffer fails with an MTLCommandBufferErrorDomain code
    NoGpuRung,           // Metal allows it and the planner has no GPU-side rung yet: the remaining work
};

enum class Problem : uint8_t {
    None,
    RenderTargetSize,
    PipelineColorFormat,
    PipelineSampleCount,
    PipelineDepthFormat,
    IndexBufferOffset,
    MemorylessAccess,
    CopyTexelSize,
    BufferRange,
    LayeredRendering,
    RasterizationRateMap,
    SamplePositions,
    NonUniformThreadgroups,
    ChannelLost,
    FetchWithoutStorageForm,
    RotatedTarget,
    TwoPlaneExtendedRange,
    StoredMultisample,
    PlanarTarget,
    SamplerAddressMode,
    ResolveFormat,
    MipFormat,
    ConversionState,
    CompressedCopy,
    Count
};

constexpr const char *kProblemNames[] = {"none",
                                         "render_target_size",
                                         "pipeline_color_format",
                                         "pipeline_sample_count",
                                         "pipeline_depth_format",
                                         "index_buffer_offset",
                                         "memoryless_access",
                                         "copy_texel_size",
                                         "buffer_range",
                                         "layered_rendering",
                                         "rasterization_rate_map",
                                         "sample_positions",
                                         "non_uniform_threadgroups",
                                         "channel_lost",
                                         "fetch_without_storage_form",
                                         "rotated_target",
                                         "two_plane_extended_range",
                                         "stored_multisample",
                                         "planar_target",
                                         "sampler_address_mode",
                                         "resolve_format",
                                         "mip_format",
                                         "conversion_state",
                                         "compressed_copy"};
static_assert(sizeof kProblemNames / sizeof *kProblemNames == size_t(Problem::Count), "a name for each problem");

struct Finding {
    Verdict verdict = Verdict::Plannable;
    Problem problem = Problem::None;
    uint32_t encoder = kNone;
    long commandBufferError = 0;  // MTLCommandBufferError where verdict is CommandBufferError

    bool plannable() const
    {
        return verdict == Verdict::Plannable;
    }
};

// What the device answers for the query-gated features (raw_device_answers.h per kind)
struct Answers {
    bool layeredRendering = false, rateMaps = false, samplePositions = false, nonUniformThreadgroups = false;
};

struct ChannelState {
    bool lost = false;
};

Finding validate(const Recording &r, const Built &built, const Answers &answers, const ChannelState &channel);

}  // namespace raw::plan
