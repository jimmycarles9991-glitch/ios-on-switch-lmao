// SPDX-License-Identifier: MIT
// The graph builder's working state: the node being filled, and what this command buffer already refreshed (the shadow
// images, stand-ins and converted images today's encode tracks in fresh, standInFresh and the converted list)
#pragma once
#include "plan_engine.h"
#include <vector>

namespace raw::plan {

class Builder {
public:
    Builder(const Recording &r, const Caps &caps, WorkGraph &g)
        : r(r), caps(caps), g(g), fresh(r.resources.size()), standInFresh(r.resources.size()),
          converted(r.resources.size()), codes(r.resources.size()), planesWritten(r.resources.size())
    {}

    const Recording &r;
    const Caps &caps;
    WorkGraph &g;
    std::vector<uint8_t> fresh, standInFresh, converted, codes, planesWritten;
    std::vector<uint32_t> unplanned;  // nodes no rule could place (their ladders had no selectable rung)

    uint32_t open(NodeKind kind, Engine engine, Rule rule, uint32_t source = kNone)
    {
        Node n;
        n.kind = kind;
        n.engine = engine;
        n.rule = rule;
        n.source = source;
        n.uses.first = uint32_t(g.uses.size());
        n.runs.first = uint32_t(g.runs.size());
        g.nodes.push_back(n);
        return uint32_t(g.nodes.size() - 1);
    }

    void use(uint32_t slot, Path path, bool write, Stage stage = Stage::Fragment, SubRange range = {})
    {
        if (slot == kNone)
            return;
        Use u;
        u.slot = slot;
        u.path = path;
        u.write = write;
        u.stage = stage;
        u.range = range;
        g.uses.push_back(u);
        ++g.nodes.back().uses.count;
    }

    DrawRun &run(OwnState own)
    {
        DrawRun r;
        r.own = own;
        r.draws.count = 1;
        g.runs.push_back(r);
        ++g.nodes.back().runs.count;
        return g.runs.back();
    }

    const ResourceFacts &facts(uint32_t slot) const
    {
        return r.resources[slot];
    }

    // A one-draw pass of the pack's own state from `from` into `to`, as encodeRawAttempt's lambdas draw them
    uint32_t ownPass(Rule rule, OwnState own, uint32_t from, uint32_t to, SubRange fromRange = {},
                     SubRange toRange = {})
    {
        const uint32_t n = open(NodeKind::Convert, Engine::ThreeD, rule);
        g.nodes[n].from = from;
        g.nodes[n].to = to;
        g.nodes[n].fromRange = fromRange;
        g.nodes[n].toRange = toRange;
        use(from, Path::Texture, false, Stage::Fragment, fromRange);
        use(to, Path::Rop, true, Stage::Fragment, toRange);
        run(own);
        const uint16_t format = facts(own == OwnState::ExtendedRange ? from : to).format;
        if (!caps.selectable(rule) || !caps.own.available(own, format))
            unplanned.push_back(n);
        return n;
    }

    uint32_t copy(Rule rule, uint32_t from, uint32_t to, NodeKind kind = NodeKind::Convert)
    {
        const uint32_t n = open(kind, Engine::Copy, rule);
        g.nodes[n].from = from;
        g.nodes[n].to = to;
        use(from, Path::Copy, false, Stage::Transfer);
        use(to, Path::Copy, true, Stage::Transfer);
        if (!caps.selectable(rule))
            unplanned.push_back(n);
        return n;
    }
};

// Implicit work (plan_implicit.cpp): each is a rule the encode loop runs today as a lambda
void beforePass(Builder &b, const Encoder &e, const std::vector<uint32_t> &sampled);
void afterPass(Builder &b, const Encoder &e);
void beforeDispatch(Builder &b, const Dispatch &d);
void afterDispatch(Builder &b, const Dispatch &d);
void mipChain(Builder &b, const Transfer &t, uint32_t source);

// The texture slots a dispatch samples through an image of its own
std::vector<uint32_t> shadowedSlots(const Builder &b, const Dispatch &d);

}  // namespace raw::plan
