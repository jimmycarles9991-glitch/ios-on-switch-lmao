// SPDX-License-Identifier: MIT
// The pass-form device tests' reader of what gm20b::Device submitted (the kernel model keeps each job's push words): the
// methods in order, the copy engine's copies, and a fixture that encodes one pass.
#pragma once
#include "device_harness.h"
#include "../mixed_samples.h"
#include "../pack.h"
#include "../record.h"
#include "../turn_copy.h"
#include "../turned_pass.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

constexpr uint32_t kRGBA8 = 70, kR32Float = 55, kDepth16 = 250, kDepth32Float = 252, kZF32 = 0x0a, kTriangleStrip = 4;
constexpr uint32_t kCeLaunch = 0x0300, kCeInUpper = 0x0400, kCeInLower = 0x0404, kCeOutUpper = 0x0408,
                   kCeOutLower = 0x040c, kCeLineLength = 0x0418, kCeRemapComponents = 0x0708;

struct Method {
    uint32_t subc, method, value;
};

struct Copy {
    uint64_t from, to;
    uint32_t remap;
};

inline std::vector<Method> submitted(size_t first)
{
    std::vector<Method> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                out.push_back({subc, m, w});
                left--;
                if (op == 1 || op == 5)
                    m += 4;
                if (op == 5)
                    op = 3;
                continue;
            }
            if (!w)
                continue;
            op = w >> 29;
            subc = w >> 13 & 7;
            m = (w & 0xfff) << 2;
            if (op == 4)
                out.push_back({subc, m, w >> 16 & 0x1fff});
            else
                left = w >> 16 & 0x1fff;
        }
    }
    return out;
}

// A launch of no line length moves nothing: the copy engine's ordering launch after a group of copies
inline std::vector<Copy> copies(const std::vector<Method> &ms)
{
    std::vector<Copy> out;
    uint64_t in = 0, to = 0;
    uint32_t remap = 0, length = 0;
    for (const Method &x : ms) {
        if (x.subc != gm20b::SUBC_COPY)
            continue;
        if (x.method == kCeRemapComponents)
            remap = x.value;
        else if (x.method == kCeLineLength)
            length = x.value;
        else if (x.method == kCeInUpper)
            in = (in & 0xffffffffull) | uint64_t(x.value) << 32;
        else if (x.method == kCeInLower)
            in = (in & ~0xffffffffull) | x.value;
        else if (x.method == kCeOutUpper)
            to = (to & 0xffffffffull) | uint64_t(x.value) << 32;
        else if (x.method == kCeOutLower)
            to = (to & ~0xffffffffull) | x.value;
        else if (x.method == kCeLaunch && in && to && length)
            out.push_back({in, to, remap});
    }
    return out;
}

inline std::vector<uint32_t> values(const std::vector<Method> &ms, uint32_t method)
{
    std::vector<uint32_t> out;
    for (const Method &x : ms)
        if (x.subc == gm20b::SUBC_3D && x.method == method)
            out.push_back(x.value);
    return out;
}

inline uint32_t lastOf(const std::vector<Method> &ms, uint32_t method)
{
    const std::vector<uint32_t> v = values(ms, method);
    return v.empty() ? ~0u : v.back();
}

inline uint64_t addressOf(const std::vector<Method> &ms, uint32_t upper)
{
    return uint64_t(lastOf(ms, upper)) << 32 | lastOf(ms, upper + 4);
}

inline size_t copiesBetween(const std::vector<Copy> &cs, uint64_t from, uint64_t to)
{
    return size_t(std::count_if(cs.begin(), cs.end(), [&](const Copy &c) {
        return c.from == from && c.to == to;
    }));
}

inline size_t touching(const std::vector<Copy> &cs, uint64_t va, uint64_t bytes)
{
    return size_t(std::count_if(cs.begin(), cs.end(), [&](const Copy &c) {
        return (c.from >= va && c.from < va + bytes) || (c.to >= va && c.to < va + bytes);
    }));
}

class DevicePassForms : public harness::DeviceTest {
protected:
    // An own state of the pack named `name`, keyed as the pack keys it
    void ownState(const char *name, const char *key)
    {
        gm20b::RuntimeRecord r;
        r.keyString = key;
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }

    gm20b::Pipeline *state(uint64_t key, const char *keyString)
    {
        gm20b::RuntimeRecord r;
        r.keyString = keyString;
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = "vs";
        r.programs[1].id = std::string("fs") + keyString;
        r.pipeline.keyHash = key;
        r.blob = {0x1a00, 0x1111};
        std::string why;
        return dev.addRecord(r, false, why);
    }

    // One pass over `p`, `body` encoding into it: what the command buffer submitted, empty with *why where it was refused
    template <class Body> std::vector<Method> pass(const gm20b::PassDesc &p, Body body, std::string *why = nullptr)
    {
        gm20b::CommandBuffer *cb = dev.begin();
        if (!cb)
            return {};
        const size_t first = kernel_model::model.jobs.size();
        const char *rule = nullptr;
        gm20b::RenderEncoder *e = cb->render(p, &rule);
        if (!e) {
            cb->abandon();
            if (why)
                *why = rule ? rule : "no pass";
            return {};
        }
        const bool ok = body(*e, &rule);
        e->end();
        uint32_t seqno = 0;
        if (!ok || !cb->commit(seqno)) {
            cb->abandon();
            if (why)
                *why = rule ? rule : "not committed";
            return {};
        }
        return submitted(first);
    }

    std::vector<Method> pass(const gm20b::PassDesc &p, std::string *why = nullptr)
    {
        return pass(
            p,
            [](gm20b::RenderEncoder &, const char **) {
                return true;
            },
            why);
    }

    static gm20b::ColorAttachment kept(gm20b::Texture &t, decltype(gm20b::ColorAttachment::load) load)
    {
        gm20b::ColorAttachment a;
        a.texture = &t;
        a.load = load;
        a.store = true;
        return a;
    }
};

class NoDraws final : public gm20b::DrawReplay {
public:
    bool any() const override
    {
        return false;
    }

    bool atFrameResolution() const override
    {
        return false;
    }

    bool replay(gm20b::RenderEncoder &, const char **) override
    {
        return true;
    }
};

}  // namespace
