// SPDX-License-Identifier: BSD-2-Clause-Patent
// A contact's reported position (SwitchTouch.cpp), in report-frame pixels: the filtering Apple's touch screens get from
// MultitouchHID before IOHIDFamily and UIKit see a contact (21H461 MTParserPath::filterContactForScreenUI), which UIKit's
// gesture thresholds assume. Our path has none (IOHIDEventDriver hands the descriptor's X and Y straight on), and the
// FTM4's centroid moves while a fingertip lands and lifts: on 11-06, still taps travelled 6 px on average (11 at most)
// in the middle of the screen, 10 (24) on the status bar, and keyboard taps of 60-85 ms 15 (60), of which the first
// 30 ms held only 4 (18). UIKit's thresholds are points: at 1.25 px per point, 10 pt (UIPanGestureRecognizer) is
// 12.5 px, 13 pt (where an edge swipe's back-projected start must land) 16 px and 15 pt (edge-swipe hysteresis) 19 px.
// A key in the iPad keyboard's flick state takes its alternate glyph once the touch is a quarter of the key's height
// below where it landed (21H461 -[UIKeyboardLayoutStar touchDragged:executionContext:]). That travel became Control
// Centre and Notification Centre pulls, Spotlight pull-downs, keyboard flicks and dismissals, and a finger paused for the
// app switcher never read as still.
//
// MultitouchHID's mechanisms, in millimetres and seconds so they hold at any UI scale:
//  * Instability: while the contact is settling (all of a change at touch-down fading to none at kSettleUs, and a fifth
//    of it to none at kSettleTailUs: MTParserPath::computeDebounceTimeInstability, with the times and fraction of one
//    of its parser types) or its strength is changing fast (a press, a lift: MTParserPath::computeZSignalInstability;
//    kStableRate and kUnstableRate are ours), position changes are not motion. Here they are kept in an offset instead
//    of reported, and the offset is paid back only while the finger visibly moves (at most half of each step), so a
//    tap reports no travel and a drag that starts during a press loses nothing and never jumps.
//  * Hysteresis: the reported point is a drag centre that follows the finger only once it is kHysteresis away on an
//    axis (MTParserPath::updateHysteresisDragCenter_mm), so a held finger's jitter is no velocity.
//
// Where the FTM4 differs from Apple's sensor:
//  * Its strength wobbles while a finger slides (a sample-to-sample change of 14/s at the median and 51/s at p95 in the
//    11-06 traces), inside the strength-rate band, so once a contact is a pan (its reported point kPanGate from where it
//    landed) the rate term stops: it froze a quarter of the steps of every drag, left flicks 85 px short of the finger
//    at their lift and threw them at 0.6 of their speed. A pan's lift is a drop of its strength below its recent level
//    instead (kLiftFrom to kLiftTo), held back only as far as the finger was still, so a drag that stops and lifts does
//    not throw the content and a flick keeps its throw.
//  * Its centroid creeps about 1 px per scan for the first ~50 ms at steady strength. Until the contact first leaves
//    kCatchUpRadius, its point stays where it landed within kStillRadius and catches up smoothly between the two, so a
//    tap's creep never reaches a threshold above.
#pragma once
#include <stdint.h>
#include "../../kits/SwitchMath/SwitchMath.h"

namespace switch_touch_filter {

constexpr int32_t kQ = 16;  // positions in 1/16 report-frame pixel

// The OLED panel: 1280 x 720 px over 155 x 87 mm (8.26 px per mm).
constexpr int32_t kPixelsPerMeter = 1280 * 1000 / 155;

constexpr int32_t fromMicrometers(int32_t um)
{
    return static_cast<int32_t>(static_cast<int64_t>(um) * kPixelsPerMeter * kQ / 1000000);
}

constexpr int32_t kHysteresis = fromMicrometers(300);
constexpr uint32_t kSettleUs = 20000, kSettleTailUs = 60000;
// Strength change, relative, per second: instability rises from nothing at the first to full at the second.
constexpr uint32_t kStableRate = 12, kUnstableRate = 32;
constexpr uint32_t kFull = 256;  // instability 1.0

// UIKit's 10 pt pan threshold at 1.25 px per point: 12.5 px, 1.5 mm.
constexpr int32_t kPanGate = fromMicrometers(1500);
constexpr int32_t kStillRadius = fromMicrometers(800), kCatchUpRadius = fromMicrometers(3000);
// The still radius lies inside UIKit's pan threshold, so creep under it is never a pan. Past it the point gains on the
// finger until kCatchUpRadius: a pan starts when the finger is 1.9 mm out instead of 1.5 mm, and nothing trails after.
static_assert(kStillRadius < 125 * kQ / 10, "the still radius stays inside UIKit's 10 pt pan threshold at 1.25 px/pt");
// A pan's strength below its recent level by kLiftFrom (none of the step held back) to kLiftTo (all of it), as far as
// the finger was still: under kFlickSpeed (150 mm/s, 1/kQ px per ms) over the last kSpeedUs.
constexpr uint32_t kLiftFrom = kFull * 3 / 10, kLiftTo = kFull * 6 / 10;
constexpr uint32_t kFlickSpeed = static_cast<uint32_t>(fromMicrometers(150)), kSpeedUs = 20000;

constexpr uint32_t ramp(uint64_t value, uint64_t from, uint64_t to)
{
    if (value <= from) {
        return 0;
    }
    if (value >= to) {
        return kFull;
    }
    return static_cast<uint32_t>((value - from) * kFull / (to - from));
}

constexpr uint32_t settling(uint64_t sinceDownUs)
{
    const uint32_t settled =
        sinceDownUs >= kSettleUs ? 0 : static_cast<uint32_t>((kSettleUs - sinceDownUs) * kFull / kSettleUs);
    const uint32_t tail = sinceDownUs >= kSettleTailUs
                              ? 0
                              : static_cast<uint32_t>((kSettleTailUs - sinceDownUs) * kFull / kSettleTailUs / 5);
    return settled > tail ? settled : tail;
}

// 0 (stable) .. kFull: how much of a position change since the last sample is not finger motion, before a pan.
constexpr uint32_t instability(uint64_t sinceDownUs, uint64_t sinceLastUs, uint32_t z, uint32_t lastZ)
{
    const uint32_t settle = settling(sinceDownUs);
    const uint32_t larger = z > lastZ ? z : lastZ;
    const uint32_t change = z > lastZ ? z - lastZ : lastZ - z;
    // change / larger per sinceLastUs, in units of 1/kFull per second
    const uint64_t rate =
        larger && sinceLastUs ? static_cast<uint64_t>(change) * 1000000 * kFull / larger / sinceLastUs : 0;
    const uint32_t pressing =
        ramp(rate, static_cast<uint64_t>(kStableRate) * kFull, static_cast<uint64_t>(kUnstableRate) * kFull);
    return settle > pressing ? settle : pressing;
}

// 0 .. kFull: how far a pan is lifting, from its strength in 1/kQ against its recent level.
constexpr uint32_t lifting(uint32_t z, uint32_t level)
{
    return z < level ? ramp(static_cast<uint64_t>(level - z) * kFull / level, kLiftFrom, kLiftTo) : 0;
}

struct Axis {
    int32_t raw, offset, centre;  // 1/kQ px; offset = raw - (what the finger's motion has earned)

    void begin(int32_t px)
    {
        raw = centre = px * kQ;
        offset = 0;
    }

    int32_t follow() const
    {
        return raw - offset;
    }

    void absorb(int32_t px, uint32_t unstable)
    {
        const int32_t now = px * kQ;
        const int32_t delta = now - raw;
        const int32_t target = raw - offset;
        raw = now;
        offset += static_cast<int32_t>(static_cast<int64_t>(delta) * unstable / kFull);
        const int32_t moved = raw - offset - target;
        if (moved && offset) {
            int32_t repay = (moved < 0 ? -moved : moved) / 2;
            int32_t const owed = offset < 0 ? -offset : offset;
            repay = repay < owed ? repay : owed;
            offset += offset < 0 ? repay : -repay;
        }
    }

    void track(int32_t to)
    {
        if (to - centre > kHysteresis) {
            centre = to - kHysteresis;
        } else if (centre - to > kHysteresis) {
            centre = to + kHysteresis;
        }
    }

    int32_t pixel() const
    {
        return (centre + (centre >= 0 ? kQ / 2 : -kQ / 2)) / kQ;
    }
};

struct Contact {
    Axis x, y;
    uint32_t z;
    uint64_t downUs, lastUs;
    int32_t downX, downY;  // 1/kQ px
    int32_t toX, toY;      // what the drag centre last followed, 1/kQ px
    bool panning, escaped;
    uint32_t level;  // the strength's recent level, 1/kQ
    uint32_t speed;  // the finger's while not lifting, 1/kQ px per ms

    void begin(int32_t px, int32_t py, uint32_t strength, uint64_t us)
    {
        x.begin(px);
        y.begin(py);
        z = strength;
        downUs = lastUs = us;
        downX = toX = px * kQ;
        downY = toY = py * kQ;
        panning = escaped = false;
        level = strength * kQ;
        speed = 0;
    }

    // True when the reported pixel changed.
    bool sample(int32_t px, int32_t py, uint32_t strength, uint64_t us)
    {
        const int32_t wasX = x.pixel();
        const int32_t wasY = y.pixel();
        const uint64_t sinceLastUs = us - lastUs;
        const uint32_t lift = lifting(strength * kQ, level);
        const int64_t stepX = static_cast<int64_t>(px) * kQ - x.raw;
        const int64_t stepY = static_cast<int64_t>(py) * kQ - y.raw;
        const uint32_t step = switch_isqrt_u64(static_cast<uint64_t>(stepX * stepX + stepY * stepY));
        noteSpeed(step, sinceLastUs, lift);
        uint32_t unstable = 0;
        if (panning) {
            const uint32_t still = speed >= kFlickSpeed ? 0 : kFull - speed * kFull / kFlickSpeed;
            const uint32_t settle = settling(us - downUs);
            const uint32_t lifted = lift * still / kFull;
            unstable = settle > lifted ? settle : lifted;
        } else {
            unstable = instability(us - downUs, sinceLastUs, strength, z);
        }
        x.absorb(px, unstable);
        y.absorb(py, unstable);
        int32_t nextX = x.follow();
        int32_t nextY = y.follow();
        holdNearTouchDown(nextX, nextY);
        limitStep(nextX, nextY, step);
        x.track(toX = nextX);
        y.track(toY = nextY);
        const int64_t fromDownX = x.centre - downX;
        const int64_t fromDownY = y.centre - downY;
        if (fromDownX * fromDownX + fromDownY * fromDownY >= static_cast<int64_t>(kPanGate) * kPanGate)
            panning = true;
        level = static_cast<uint32_t>(static_cast<int64_t>(level) +
                                      (static_cast<int64_t>(strength) * kQ - static_cast<int64_t>(level)) / 4);
        z = strength;
        lastUs = us;
        return x.pixel() != wasX || y.pixel() != wasY;
    }

private:
    // Decays over kSpeedUs; a lifting sample's step is no speed of the finger's.
    void noteSpeed(uint64_t step, uint64_t sinceLastUs, uint32_t lift)
    {
        const uint64_t now = sinceLastUs ? step * 1000 / sinceLastUs : speed;
        speed = static_cast<uint32_t>((static_cast<uint64_t>(speed) * kSpeedUs + (lift ? 0 : now * sinceLastUs)) /
                                      (sinceLastUs + kSpeedUs));
    }

    // The offset's payback and the catch-up past kStillRadius together never move the point more than 1.5 of the
    // finger's step.
    void limitStep(int32_t &nextX, int32_t &nextY, uint32_t step) const
    {
        const int64_t dx = nextX - toX;
        const int64_t dy = nextY - toY;
        const int64_t most = static_cast<int64_t>(step) * 3 / 2;
        const int64_t d = switch_isqrt_u64(static_cast<uint64_t>(dx * dx + dy * dy));
        if (d <= most)
            return;
        nextX = toX + static_cast<int32_t>(dx * most / d);
        nextY = toY + static_cast<int32_t>(dy * most / d);
    }

    void holdNearTouchDown(int32_t &nextX, int32_t &nextY)
    {
        if (escaped)
            return;
        const int64_t dx = nextX - downX;
        const int64_t dy = nextY - downY;
        const int32_t d = static_cast<int32_t>(switch_isqrt_u64(static_cast<uint64_t>(dx * dx + dy * dy)));
        if (d >= kCatchUpRadius) {
            escaped = true;
            return;
        }
        const int64_t kept = d <= kStillRadius ? 0
                                               : static_cast<int64_t>(d - kStillRadius) * kCatchUpRadius /
                                                     (kCatchUpRadius - kStillRadius);
        nextX = downX + (d ? static_cast<int32_t>(dx * kept / d) : 0);
        nextY = downY + (d ? static_cast<int32_t>(dy * kept / d) : 0);
    }
};

}  // namespace switch_touch_filter
