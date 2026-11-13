// SPDX-License-Identifier: MIT
// The work graph: what a command buffer executes, each node on one engine with its uses, its dependency edges and the
// barriers those edges need. Every node names the rule that made it.
#pragma once
#include "plan_recording.h"
#include <array>
#include <vector>

namespace raw::plan {

enum class NodeKind : uint8_t {
    RenderPass,
    Clear,
    Copy,
    Fill,
    Blit,
    Resolve,
    MipChain,
    Convert,
    Dispatch,
    Present,
    Wait,
    Signal,
    Count
};

enum class Rule : uint8_t {
    Recorded,
    CopyEngine,
    CopyEngineCompressed,
    CopyTwoD,
    FillCopyEngine,
    ClearThreeD,
    ClearTwoD,
    ResolveDraw,
    ResolveTwoD,
    MipTwoD,
    MipTwoDProbed,
    MipDraw,
    PlanarCodes,
    PlanarStandInLoad,
    PlanarStandInStore,
    YcbcrPlaneFill,
    YcbcrImage,
    ShadowRefresh,
    ExtendedRangeStandIn,
    ComputeShadowLoad,
    ComputeShadowStore,
    FetchBarrier,
    FetchBlend,
    FetchIterated,
    FetchInterlock,
    PassPixelBarrier,
    EntryNoDrain,
    SwitchDrain,
    Count
};

constexpr const char *kRuleNames[] = {"recorded",
                                      "copy.ce",
                                      "copy.ce_compressed",
                                      "copy.twod",
                                      "fill.ce",
                                      "clear.3d",
                                      "clear.twod",
                                      "resolve.draw",
                                      "resolve.twod",
                                      "mip.twod",
                                      "mip.twod_probed",
                                      "mip.draw",
                                      "planar.codes",
                                      "planar.standin_load",
                                      "planar.standin_store",
                                      "ycbcr.plane_fill",
                                      "ycbcr.image",
                                      "shadow.refresh",
                                      "xr.standin",
                                      "compute.shadow_load",
                                      "compute.shadow_store",
                                      "fetch.barrier",
                                      "fetch.blend",
                                      "fetch.iterated",
                                      "fetch.interlock",
                                      "barrier.pass_pixel",
                                      "barrier.entry_no_drain",
                                      "barrier.switch_drain"};
static_assert(sizeof kRuleNames / sizeof *kRuleNames == size_t(Rule::Count), "a name for each rule");

// The barrier vocabulary, one bit per kit sequence (kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h). EngineSwitch is
// never emitted: it records that the subchannel change before a node drained the engine left behind.
enum Barrier : uint16_t {
    TiledCacheFlush = 1u << 0,
    WaitForIdle = 1u << 1,
    FlushShaderData = 1u << 2,
    InvalidateShaderData = 1u << 3,
    InvalidateConstants = 1u << 4,
    InvalidateInstructions = 1u << 5,
    InvalidateTextureData = 1u << 6,
    PixelShaderBarrier = 1u << 7,
    InvalidateTextureHeaders = 1u << 8,
    InvalidateSamplers = 1u << 9,
    SetReference = 1u << 10,
    CopyNonPipelined = 1u << 11,
    EngineSwitch = 1u << 12,
};

using Barriers = uint16_t;

constexpr Barriers kShaderCaches =
    FlushShaderData | InvalidateShaderData | InvalidateConstants | InvalidateInstructions;

// The program form a run draws with (raw_pack.h PackForm), and the fetch ladder's rung per colour (PackFetchRung)
enum class Variant : uint8_t {
    Whole,
    Split,
    Storage,
    Barrier
};
enum class Rung : uint8_t {
    None,
    Barrier,
    Blend,
    Iterated,
    Interlock
};

// The pack's own states the implicit passes draw with (raw::Device standIn/resolve/ycbcr/xr/mip pipelines)
enum class OwnState : uint8_t {
    None,
    Resolve,
    Mip,
    ExtendedRange,
    Ycbcr,
    StandInLoad,
    StandInStore
};

struct DrawRun {
    uint32_t state = kNone;
    uint32_t program = kNone;
    OwnState own = OwnState::None;
    Variant variant = Variant::Whole;
    std::array<Rung, 8> rung{};
    std::array<Rule, 8> rungRule{};
    Span draws;
    Barriers requested = 0;  // what a memoryBarrier the client recorded asks for
    Barriers before = 0;
};

struct Node {
    NodeKind kind = NodeKind::RenderPass;
    Engine engine = Engine::ThreeD;
    Rule rule = Rule::Recorded;
    uint32_t source = kNone;  // the recording's encoder, transfer, dispatch or boundary; kNone for implicit work
    uint32_t pass = kNone;    // RenderPass: the recording's pass, kNone for an implicit pass
    uint32_t from = kNone, to = kNone;
    SubRange fromRange, toRange;
    Span uses, after, runs;
    bool continues = false;  // fused into the RenderPass node before it: same targets, no barrier, no new prologue
    uint64_t key = 0;
    Barriers before = 0;
};

// An MTLFence's waitForFence: in node `waiter`, of the update node `updater` made before it. A fence orders what its encoders
// touch that no use names (an argument buffer's resources, an untracked heap's), so the wait takes a full drain.
struct FenceEdge {
    uint32_t waiter = kNone, updater = kNone;
};

constexpr Barriers kFenceWait =
    WaitForIdle | FlushShaderData | InvalidateShaderData | InvalidateConstants | InvalidateTextureData;

struct WorkGraph {
    std::vector<Node> nodes;
    std::vector<Use> uses;
    std::vector<uint32_t> edges;
    std::vector<DrawRun> runs;
    std::vector<FenceEdge> fences;
    Barriers entry = 0, exit = 0;
};

constexpr const char *kNodeKindNames[] = {"RenderPass", "Clear",   "Copy",     "Fill",    "Blit", "Resolve",
                                          "MipChain",   "Convert", "Dispatch", "Present", "Wait", "Signal"};
static_assert(sizeof kNodeKindNames / sizeof *kNodeKindNames == size_t(NodeKind::Count), "a name for each kind");

}  // namespace raw::plan
