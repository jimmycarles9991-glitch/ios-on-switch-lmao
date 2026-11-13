// Rail reports for the host tests, built on the captured Joy-Con (L) (switch_joycon_capture.h): its calibrated stick,
// its resting samples, and the noise, drift and pushes the tests add to them.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "../SwitchJoyConStick.h"
#include "switch_joycon_capture.h"

namespace switch_joycon_fixture {

using Report = std::array<uint8_t, 49>;

inline void putStick(Report &r, bool left, unsigned x, unsigned y)
{
    const unsigned at = left ? 6 : 9;
    r[at] = static_cast<uint8_t>(x & 0xff);
    r[at + 1] = static_cast<uint8_t>((x >> 8) | ((y & 0x0f) << 4));
    r[at + 2] = static_cast<uint8_t>(y >> 4);
}

// A 0x30 report of a Joy-Con at rest: full battery, not charging, no buttons; the other side's stick reads 0, 0.
inline Report report(bool left, unsigned x, unsigned y, uint8_t timer = 0)
{
    Report r{};
    r[0] = 0x30;
    r[1] = timer;
    r[2] = 0x80;
    putStick(r, left, x, y);
    return r;
}

// Subcommand 0x10's reply data for `read`: address, size, bytes.
inline std::vector<uint8_t> spiReply(bool left, unsigned read, const uint8_t *bytes)
{
    const uint32_t address = switch_joycon_stick_address(left, read);
    const uint8_t size = switch_joycon_stick_size(read);
    std::vector<uint8_t> data = {static_cast<uint8_t>(address), static_cast<uint8_t>(address >> 8), 0, 0, size};
    data.insert(data.end(), bytes, bytes + size);
    return data;
}

// The captured Joy-Con (L)'s stick, read as the rail reads it: factory calibration, erased user calibration, parameters.
inline switch_joycon_stick capturedStick()
{
    switch_joycon_stick s{};
    switch_joycon_stick_defaults(&s, true);
    const std::vector<uint8_t> factory = spiReply(true, SWITCH_JOYCON_STICK_FACTORY, switch_joycon_capture::kFactory);
    const uint8_t erased[11] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const std::vector<uint8_t> user = spiReply(true, SWITCH_JOYCON_STICK_USER, erased);
    const std::vector<uint8_t> params = spiReply(true, SWITCH_JOYCON_STICK_PARAMS, switch_joycon_capture::kParams);
    switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_FACTORY, factory.data(), factory.size());
    switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_USER, user.data(), user.size());
    switch_joycon_stick_reply(&s, SWITCH_JOYCON_STICK_PARAMS, params.data(), params.size());
    return s;
}

// A deterministic jitter of up to +-amplitude counts.
struct Jitter {
    uint32_t state = 12345;

    int next(int amplitude)
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 16) % static_cast<uint32_t>(2 * amplitude + 1)) - amplitude;
    }
};

// `count` resting reports, 15 ms apart: the captured samples over and over, each with up to +-jitter more counts, all
// drifting linearly by drift_x, drift_y counts over the run.
inline std::vector<Report> resting(size_t count, int jitter = 0, int drift_x = 0, int drift_y = 0)
{
    std::vector<Report> out;
    Jitter noise;
    constexpr size_t kSamples = sizeof(switch_joycon_capture::kResting) / sizeof(switch_joycon_capture::kResting[0]);
    for (size_t i = 0; i < count; ++i) {
        const auto &sample = switch_joycon_capture::kResting[i % kSamples];
        const int dx = count > 1 ? drift_x * static_cast<int>(i) / static_cast<int>(count - 1) : 0;
        const int dy = count > 1 ? drift_y * static_cast<int>(i) / static_cast<int>(count - 1) : 0;
        out.push_back(report(true, static_cast<unsigned>(sample.x + dx + noise.next(jitter)),
                             static_cast<unsigned>(sample.y + dy + noise.next(jitter)), static_cast<uint8_t>(i)));
    }
    return out;
}

// From rest to `x` on the stick's x axis over `steps` reports, then held there with the resting jitter.
inline std::vector<Report> push(int x, int steps, size_t held)
{
    std::vector<Report> out;
    const int restX = switch_joycon_capture::kResting[0].x, restY = switch_joycon_capture::kResting[0].y;
    for (int i = 1; i <= steps; ++i)
        out.push_back(
            report(true, static_cast<unsigned>(restX + (x - restX) * i / steps), static_cast<unsigned>(restY)));
    for (const Report &r : resting(held)) {
        const int jitterX = (r[6] | (r[7] & 0x0f) << 8) - restX;
        out.push_back(report(true, static_cast<unsigned>(x + jitterX), static_cast<unsigned>(r[7] >> 4 | r[8] << 4)));
    }
    return out;
}

}  // namespace switch_joycon_fixture
