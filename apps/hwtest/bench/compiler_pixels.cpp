// SPDX-License-Identifier: MIT
#include "apps/hwtest/bench/compiler_pixels.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace bench::compiler {
std::string difference(const Attachment &plane, const std::vector<uint8_t> &a, const std::vector<uint8_t> &b,
                       const char *labelA, const char *labelB)
{
    const size_t samples = size_t(plane.width) * plane.height * plane.samples;
    if (a.size() != b.size() || !samples || a.empty() || a.size() % samples)
        return "invalid attachment extents";
    const auto match = std::mismatch(a.begin(), a.end(), b.begin());
    if (match.first == a.end())
        return {};
    const size_t bpp = a.size() / samples, offset = (match.first - a.begin()) / bpp;
    const size_t pixel = offset / plane.samples;
    size_t x = pixel % plane.width, y = pixel / plane.width, sample = offset % plane.samples;
    if (plane.depth && plane.samples == 4) {
        const size_t gx = offset % (2 * plane.width), gy = offset / (2 * plane.width);
        x = gx / 2;
        y = gy / 2;
        sample = (gx & 1) + 2 * (gy & 1);
    }
    std::ostringstream s;
    if (plane.depth)
        s << "depth/stencil";
    else
        s << "color " << plane.index;
    s << " (" << x << "," << y << ") sample " << sample;
    auto value = [&](const char *label, const std::vector<uint8_t> &bytes) {
        s << " " << label << " " << std::hex << std::setfill('0');
        for (size_t i = 0; i < bpp; ++i)
            s << std::setw(2) << unsigned(bytes[offset * bpp + i]);
    };
    value(labelA, a);
    value(labelB, b);
    return s.str();
}
}  // namespace bench::compiler
