// SPDX-License-Identifier: GPL-2.0-only
// floorKhz's conversion and ISO efficiency table are NVIDIA's (L4T drivers/platform/tegra/mc/emc_bwmgr-t21x.c):
// Copyright (c) 2016-2019, NVIDIA CORPORATION. All rights reserved. Licensed under the GNU General Public License
// version 2, as published by the Free Software Foundation.
#pragma once
#include <stdint.h>

namespace switch_display {
struct Demand {
    uint32_t x, y, width, height;
    uint64_t bytesPerSecond;
};

inline uint64_t add(uint64_t a, uint64_t b)
{
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}

inline uint64_t scaled(uint64_t bytes, uint32_t source, uint32_t destination)
{
    if (!destination)
        return UINT64_MAX;
    if (source <= destination)
        return bytes;
    if (source && bytes > UINT64_MAX / source)
        return UINT64_MAX;
    const uint64_t product = bytes * source;
    return product / destination + (product % destination != 0);
}

inline uint64_t peak(const Demand *planes, unsigned count)
{
    if (count > 3 || (!planes && count))
        return UINT64_MAX;
    uint64_t maximum = 0;
    for (unsigned mask = 1; mask < (1u << count); ++mask) {
        uint64_t left = 0, top = 0, right = UINT64_MAX, bottom = UINT64_MAX, bytes = 0;
        for (unsigned p = 0; p < count; ++p) {
            if (!(mask & (1u << p)))
                continue;
            const auto &d = planes[p];
            if (left < d.x)
                left = d.x;
            if (top < d.y)
                top = d.y;
            const uint64_t r = uint64_t(d.x) + d.width, b = uint64_t(d.y) + d.height;
            if (right > r)
                right = r;
            if (bottom > b)
                bottom = b;
            bytes = add(bytes, d.bytesPerSecond);
        }
        if (left < right && top < bottom && bytes > maximum)
            maximum = bytes;
    }
    return maximum;
}

// The display's isochronous floor as L4T's T21x bandwidth manager sets it (emc_bwmgr-t21x.c, GPL-2.0, NVIDIA): the rate
// is the DRAM clock (twice EMC), LPDDR4 is two channels moving 16 bytes per DRAM clock (bw_to_freq), and an isochronous
// request is divided by the efficiency of the bucket its own clock falls in (bwmgr_t210_lpddr4_iso_eff). Its 70% total
// efficiency covers non-isochronous traffic, which the ACTMON governor follows instead.
inline uint32_t isoEfficiencyPercent(uint64_t khz)
{
    constexpr uint16_t bucketMhz[] = {5,   10,  20,  30,  40,  60,  80,  100, 120, 140, 160, 180, 200,
                                      250, 300, 350, 360, 370, 380, 400, 450, 500, 550, 600, 650, 700};
    constexpr uint8_t lpddr4[] = {56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56,
                                  56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 56, 49, 45};
    unsigned bucket = sizeof(bucketMhz) / sizeof(bucketMhz[0]) - 1;
    while (bucket && bucketMhz[bucket] > khz / 1000)
        --bucket;
    return lpddr4[bucket];
}

inline uint32_t floorKhz(uint64_t bytesPerSecond)
{
    constexpr uint64_t bytesPerKhz = 16000;
    const uint64_t khz = bytesPerSecond / bytesPerKhz + (bytesPerSecond % bytesPerKhz != 0);
    const uint64_t efficiency = isoEfficiencyPercent(khz);
    const uint64_t floor = khz > UINT64_MAX / 100 ? UINT64_MAX : (khz * 100 + efficiency - 1) / efficiency;
    return floor > UINT32_MAX ? UINT32_MAX : uint32_t(floor);
}

inline unsigned row(const uint32_t *rates, unsigned count, uint64_t bytesPerSecond)
{
    const uint32_t floor = floorKhz(bytesPerSecond);
    for (unsigned r = 0; r < count; ++r)
        if (rates[r] >= floor)
            return r;
    return count;
}

class AdmissionEnvelope {
    uint64_t layer_[4] = {};

public:
    void clear()
    {
        for (auto &bytes : layer_)
            bytes = 0;
    }

    template <class Reserve>
    bool admit(const uint64_t updated[4], const uint64_t active[2], uint64_t ui, Reserve reserve)
    {
        uint64_t next[4], largest = 0, second = 0;
        const auto include = [&](uint64_t bytes) {
            if (bytes >= largest) {
                second = largest;
                largest = bytes;
            } else if (bytes > second)
                second = bytes;
        };
        for (unsigned layer = 0; layer != 4; ++layer) {
            next[layer] = updated[layer] > layer_[layer] ? updated[layer] : layer_[layer];
            include(next[layer]);
        }
        for (unsigned window = 0; window != 2; ++window)
            include(active[window]);
        if (!reserve(add(ui, add(largest, second))))
            return false;
        for (unsigned layer = 0; layer != 4; ++layer)
            layer_[layer] = next[layer];
        return true;
    }
};

class Reservation {
    uint64_t bytes_ = 0;

public:
    uint64_t bytes() const
    {
        return bytes_;
    }

    template <class Apply> bool raise(uint64_t demand, Apply apply)
    {
        if (demand <= bytes_)
            return true;
        if (apply(demand))
            return false;
        bytes_ = demand;
        return true;
    }

    template <class Apply> bool retire(uint64_t demand, Apply apply)
    {
        if (demand == bytes_)
            return true;
        if (apply(demand))
            return false;
        bytes_ = demand;
        return true;
    }
};
}  // namespace switch_display
