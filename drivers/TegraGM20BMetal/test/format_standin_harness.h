// SPDX-License-Identifier: MIT
// What the format stand-in tests (device_format_standin*_test.cpp) read back of the passes gm20b::Device submitted, and the
// pack states they give it
#pragma once
#include "device_harness.h"
#include "../format_standin.h"
#include "../pack.h"
#include "../record.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B3D.h"
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace standin {

// The copy engine's launches the command buffer submitted: source, destination, SET_REMAP_COMPONENTS, and the texels a line
// and the lines each moved
struct Launch {
    uint64_t from, to;
    uint32_t remap, width, lines;
};

struct Method {
    uint32_t method, value;
};

// Every job's words from `first` decoded: the copy engine's launches, and the 3D subchannel's methods in order
inline void decode(size_t first, std::vector<Launch> &launches, std::vector<Method> &methods)
{
    constexpr uint32_t kLaunch = 0x0300, kInUpper = 0x0400, kInLower = 0x0404, kOutUpper = 0x0408, kOutLower = 0x040c,
                       kLineLength = 0x0418, kLineCount = 0x041c, kRemap = 0x0708;
    uint64_t in = 0, to = 0;
    uint32_t remap = 0, width = 0, lines = 0;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        uint32_t op = 0, subc = 0, m = 0, left = 0;
        auto method = [&](uint32_t v) {
            if (subc == 0)
                methods.push_back({m, v});
            if (subc != gm20b::SUBC_COPY)
                return;
            if (m == kInUpper)
                in = (in & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kInLower)
                in = (in & ~0xffffffffull) | v;
            else if (m == kOutUpper)
                to = (to & 0xffffffffull) | uint64_t(v) << 32;
            else if (m == kOutLower)
                to = (to & ~0xffffffffull) | v;
            else if (m == kRemap)
                remap = v;
            else if (m == kLineLength)
                width = v;
            else if (m == kLineCount)
                lines = v;
            else if (m == kLaunch)
                launches.push_back({in, to, remap, width, lines});
        };
        for (const uint32_t w : jobs[j].words) {
            if (left) {
                method(w);
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
                method(w >> 16 & 0x1fff);
            else
                left = w >> 16 & 0x1fff;
        }
    }
}

// The colour target 0 each pass set: its address and format
inline std::vector<std::pair<uint64_t, uint32_t>> targets(const std::vector<Method> &methods)
{
    std::vector<std::pair<uint64_t, uint32_t>> out;
    uint64_t va = 0;
    for (const Method &m : methods) {
        if (m.method == NVB197_SET_COLOR_TARGET_A(0))
            va = uint64_t(m.value) << 32;
        else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
            va |= m.value;
        else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(0) && m.value)
            out.push_back({va, m.value});
    }
    return out;
}

class DeviceFormatStandIn : public harness::DeviceTest {
protected:
    void addState(const char *name, uint32_t format, uint32_t samples = 1)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=" + std::to_string(samples) + " c0=" + std::to_string(format) + " ";
        for (gm20b::CapturedProgram &p : r.programs) {
            p.image.assign(80 + 64, 0);
            p.gprs = 8;
        }
        r.programs[0].id = std::string(name) + "-vs";
        r.programs[1].id = std::string(name) + "-fs";
        r.pipeline.keyHash = gm20b::Pack::fnv(name, strlen(name));
        r.blob = {0x1a00, 0x1111};
        std::string why;
        ASSERT_NE(dev.addRecord(r, false, why), nullptr) << why;
    }
};

// remapOf: DST_X..W each a source component or NO_WRITE (6), one-byte components, the source's and destination's counts
constexpr uint32_t remap(uint32_t sources, uint32_t destinations)
{
    return 0 | 6u << 4 | 6u << 8 | 6u << 12 | (sources - 1) << 20 | (destinations - 1) << 24;
}

// The attachment a pass binds of all of `t`
inline gm20b::ColorAttachment whole(gm20b::Texture &t)
{
    gm20b::ColorAttachment a;
    a.texture = &t;
    return a;
}

}  // namespace standin
