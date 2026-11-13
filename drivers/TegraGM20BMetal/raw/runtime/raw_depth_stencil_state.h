// SPDX-License-Identifier: MIT
// MTLDepthStencilState as the GM20B's depth/stencil register values: Mesa NVK's nvk_flush_ds_state and its
// vk_to_nv9097_compare_op / vk_to_nv9097_stencil_op encodings (MIT), over the Vulkan state metal2vulkan's state.rs
// depth_stencil_state makes of a Metal state (a depth test where the compare is not Always or depth writes, a stencil test
// where either face is not a no-op). The enables only with an attachment of the aspect; masks cut to 8 bits as Mesa's
// dynamic state keeps them; references saturated at 255 (metal2vulkan render_state.rs, as Metal measured).
#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>
#include "../../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include "raw_state_segment.h"
#include "raw_zeta.h"

namespace raw {

// The methods that take `held` (what the GPU holds) to `want`, then held = want
inline void emitDepthStencil(Push &p, const DepthStencilRegs &want, DepthStencilRegs &held)
{
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        if (want.v[i] != held.v[i])
            p.m(kDepthStencilMethods[i], want.v[i]);
    held = want;
}

// MTLCompareFunction and VkCompareOp share their order; NV's OGL value is 0x200 | it
inline uint32_t depthStencilCompare(uint8_t f)
{
    return 0x200u | (f & 7u);
}

// MTLStencilOperation and VkStencilOp share their order; NV's D3D value is it + 1
inline uint32_t depthStencilOperation(uint8_t op)
{
    return (op & 7u) + 1;
}

// The two words of DepthStencilRegs a draw's references (setStencilFrontReferenceValue:backReferenceValue:) go in
constexpr uint32_t kFrontStencilReference = 8, kBackStencilReference = 15;

// What the GPU holds at a draw of a pass with a depth/stencil attachment of the aspects `depth` and `stencil`, or without one,
// under state `d` and stencil references `refFront`/`refBack`
inline DepthStencilRegs depthStencilRegs(const DepthStencilDesc &d, uint32_t refFront, uint32_t refBack, bool depth,
                                         bool stencil)
{
    DepthStencilRegs r;
    const bool depthTest = d.depthCompare != 7 || d.depthWrite;
    const bool stencilTest = !(stencilNoop(d.front) && stencilNoop(d.back));
    r.v[0] = depthTest && depth;
    r.v[1] = d.depthWrite && depth;
    r.v[2] = depthStencilCompare(d.depthCompare);
    r.v[3] = stencilTest && stencil;
    const StencilFace *faces[2] = {&d.front, &d.back};
    const uint32_t refs[2] = {refFront, refBack};
    for (uint32_t i = 0; i < 2; i++) {
        const StencilFace &s = *faces[i];
        uint32_t *ops = i ? &r.v[11] : &r.v[4];
        ops[0] = depthStencilOperation(s.fail);
        ops[1] = depthStencilOperation(s.depthFail);
        ops[2] = depthStencilOperation(s.pass);
        ops[3] = depthStencilCompare(s.compare);
        // front: ref, func mask, mask; back: ref, mask, func mask
        uint32_t *rest = i ? &r.v[kBackStencilReference] : &r.v[kFrontStencilReference];
        rest[0] = std::min(refs[i], 0xffu);
        rest[1] = (i ? s.writeMask : s.readMask) & 0xff;
        rest[2] = (i ? s.readMask : s.writeMask) & 0xff;
    }
    return r;
}

// In a pass without a depth/stencil attachment plain Metal tests a depth compare other than Always (Never: every fragment
// fails) and a stencil test against hidden ones, depth 1.0 and stencil 0 at the pass's start, kept through the pass for its
// later draws (metal2vulkan depth_stencil.rs depth_without_attachment, measured); Always with no stencil test tests nothing
inline bool testsHiddenZeta(const DepthStencilDesc &d)
{
    return d.depthCompare != 7 || !(stencilNoop(d.front) && stencilNoop(d.back));
}

// A draw whose depth/stencil tests run in its program (raw_shader_tests.h): the hardware tests nothing, depth only kept on
inline DepthStencilRegs shaderDepthStencilRegs(bool depth)
{
    auto regs = depthStencilRegs(DepthStencilDesc{}, 0, 0, depth, false);
    regs.v[0] = depth;
    return regs;
}

// A pair of aspects' registers but the stencil references, which a draw sets: a segment's words, one immediate each
inline uint32_t depthStencilStateWords(const DepthStencilRegs &r, uint32_t out[kDepthStencilRegs])
{
    Push p{out, out + kDepthStencilRegs};
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        if (i != kFrontStencilReference && i != kBackStencilReference)
            p.m(kDepthStencilMethods[i], r.v[i]);
    return uint32_t(p.at - out);
}

// The registers `want` sets that `held` (what the channel holds) lacks: by `segment` (the state's registers for the pass's
// aspects but the references) where more of them differ than a segment bind costs, then the references inline. Each
// register is an immediate, a word. True: the segment was bound.
inline bool bindDepthStencil(Push &p, std::vector<PushBody> &bodies, uint32_t *&start, const DepthStencilRegs &want,
                             DepthStencilRegs &held, const StateSegment &segment)
{
    uint32_t changed = 0;
    for (uint32_t i = 0; i < kDepthStencilRegs; i++)
        changed += want.v[i] != held.v[i];
    if (!segment || changed <= kSegmentBindWords) {
        emitDepthStencil(p, want, held);
        return false;
    }
    bindSegment(bodies, start, p.at, segment);
    const uint32_t front = held.v[kFrontStencilReference], back = held.v[kBackStencilReference];
    held = want;
    held.v[kFrontStencilReference] = front;
    held.v[kBackStencilReference] = back;
    emitDepthStencil(p, want, held);
    return true;
}

// A state's register values, made at its creation for each pair of aspects a pass's attachment can have. A draw takes its
// pass's pair and writes the references it set into them: the state's words are never derived again per draw.
class DepthStencilState {
public:
    DepthStencilState() : DepthStencilState(DepthStencilDesc())
    {}

    explicit DepthStencilState(const DepthStencilDesc &d) : desc_(d)
    {
        for (uint32_t aspects = 0; aspects < 4; aspects++) {
            byAspects_[aspects] = depthStencilRegs(d, 0, 0, aspects & 1, aspects & 2);
            depthStencilBlock(byAspects_[aspects], blocks_[aspects]);
        }
    }

    const DepthStencilDesc &desc() const
    {
        return desc_;
    }

    DepthStencilRegs regs(bool depth, bool stencil, uint32_t refFront, uint32_t refBack) const
    {
        DepthStencilRegs r = byAspects_[uint32_t(depth) | uint32_t(stencil) << 1];
        r.v[kFrontStencilReference] = std::min(refFront, 0xffu);
        r.v[kBackStencilReference] = std::min(refBack, 0xffu);
        return r;
    }

    // The words a state segment holds for a pass of these aspects; a draw adds its references (stencilReferenceWords)
    const DepthStencilBlock &block(bool depth, bool stencil) const
    {
        return blocks_[uint32_t(depth) | uint32_t(stencil) << 1];
    }

    // Metal's default state: a render encoder's until it sets one
    static const DepthStencilState &metalDefault()
    {
        static const DepthStencilState state;
        return state;
    }

    // Its registers for each pair of aspects into `heap` (whose caller serialises place())
    void place(StateSegmentHeap &heap)
    {
        uint32_t words[kDepthStencilRegs];
        for (uint32_t aspects = 0; aspects < 4; aspects++)
            segments_[aspects] = heap.place(words, depthStencilStateWords(byAspects_[aspects], words));
    }

    const StateSegment &segment(bool depth, bool stencil) const
    {
        return segments_[uint32_t(depth) | uint32_t(stencil) << 1];
    }

private:
    DepthStencilDesc desc_;
    DepthStencilRegs byAspects_[4];
    StateSegment segments_[4];
    DepthStencilBlock blocks_[4];
};

}  // namespace raw
