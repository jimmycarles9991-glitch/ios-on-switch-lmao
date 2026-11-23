// SPDX-License-Identifier: MIT
// The texture view tests' fixture (device_views_test.cpp, device_surface_views_test.cpp): a texture header's
// fields as clb097tex.h names them, and the own states a view's stand-in is drawn with.
#pragma once
#include "device_harness.h"
#include "../pack.h"
#include "../record.h"
#include <array>
#include <cstring>
#include <string>

namespace views {

constexpr uint32_t kRGBA8 = 70, kDepth16Unorm = 250, kDepth32Float = 252, kDepth32FloatStencil8 = 260,
                   kX32Stencil8 = 261, kASTC4x4 = 204;

inline uint32_t field(const std::array<uint32_t, 8> &w, unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    for (unsigned b = lo; b <= hi; b++)
        v |= uint64_t(w[b / 32] >> (b % 32) & 1) << (b - lo);
    return uint32_t(v);
}

inline uint64_t address(const std::array<uint32_t, 8> &w)
{
    return uint64_t(field(w, 63, 41)) << 9 | uint64_t(field(w, 79, 64)) << 32;
}

inline uint32_t textureType(const std::array<uint32_t, 8> &w)
{
    return field(w, 154, 151);
}

inline uint32_t minLevel(const std::array<uint32_t, 8> &w)
{
    return field(w, 227, 224);
}

inline uint32_t maxLevel(const std::array<uint32_t, 8> &w)
{
    return field(w, 231, 228);
}

inline uint32_t depthMinusOne(const std::array<uint32_t, 8> &w)
{
    return field(w, 189, 176);
}

class DeviceViews : public harness::DeviceTest {
protected:
    std::array<uint32_t, 8> header(const gm20b::Texture &t) const
    {
        return gm20b::DeviceHarness::header(dev, t.tic);
    }

    // An own state the device asks by name (the pack's conversion states), drawing format `format`
    void addOwnState(const char *name, uint32_t format)
    {
        gm20b::RuntimeRecord r;
        r.keyString = "samples=1 c0=" + std::to_string(format) + " ";
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

}  // namespace views
