// SPDX-License-Identifier: MIT
#include "../encode_profile.h"
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace {
unsigned checks;

void check(bool value)
{
    if (!value)
        std::abort();
    ++checks;
}

void spin(uint64_t ticks)
{
    const uint64_t until = gm20b::encodeTicks() + ticks;
    while (gm20b::encodeTicks() < until) {
    }
}

void outsideAnEncode()
{
    gm20b::tEncodeProfile = nullptr;
    SWITCH_METAL_ENCODE_SPAN(Kernel);
    check(gm20b::tEncodeProfile == nullptr);
}

void nested()
{
    gm20b::EncodeProfile profile;
    gm20b::tEncodeProfile = &profile;
    {
        SWITCH_METAL_ENCODE_SPAN(Draw);
        {
            SWITCH_METAL_ENCODE_SPAN(Prepare);
            SWITCH_METAL_ENCODE_BEGIN(geometry, Geometry);
            spin(1000);
            SWITCH_METAL_ENCODE_END(geometry);
            spin(1000);
        }
        SWITCH_METAL_ENCODE_SPAN(Prepare);
        spin(10);
    }
    gm20b::tEncodeProfile = nullptr;
    const auto ticks = [&](gm20b::EncodePart p) {
        return profile.ticks[size_t(p)];
    };
    const auto calls = [&](gm20b::EncodePart p) {
        return profile.calls[size_t(p)];
    };
    check(calls(gm20b::EncodePart::Draw) == 1 && calls(gm20b::EncodePart::Prepare) == 2);
    check(calls(gm20b::EncodePart::Geometry) == 1);
    check(ticks(gm20b::EncodePart::Geometry) >= 1000);
    check(ticks(gm20b::EncodePart::Prepare) >= ticks(gm20b::EncodePart::Geometry) + 1000);
    check(ticks(gm20b::EncodePart::Draw) >= ticks(gm20b::EncodePart::Prepare));
    check(profile.longestPrepare >= 2000 && profile.longestPrepare <= ticks(gm20b::EncodePart::Prepare));
    check(calls(gm20b::EncodePart::Kernel) == 0);

    gm20b::EncodeTotals totals;
    totals.add(profile, 5);
    gm20b::EncodeProfile shorter;
    shorter.calls[size_t(gm20b::EncodePart::Kernel)] = 3;
    shorter.ticks[size_t(gm20b::EncodePart::Kernel)] = 11;
    shorter.longestPrepare = 1;
    totals.add(shorter, 1);
    check(totals.encodes.load() == 2 && totals.wallTicks.load() == 6);
    check(totals.calls[size_t(gm20b::EncodePart::Prepare)].load() == 2);
    check(totals.calls[size_t(gm20b::EncodePart::Kernel)].load() == 3);
    check(totals.ticks[size_t(gm20b::EncodePart::Kernel)].load() == 11);
    check(totals.longestPrepare.load() == profile.longestPrepare);
}

// A part's self time leaves out the parts nested in it; counted events and locks land in the encoding thread's profile
void selfTimesAndEvents()
{
    gm20b::EncodeProfile profile;
    gm20b::tEncodeProfile = &profile;
    std::mutex m;
    {
        SWITCH_METAL_ENCODE_SPAN(Loop);
        spin(500);
        {
            SWITCH_METAL_ENCODE_SPAN(State);
            spin(500);
            SWITCH_METAL_ENCODE_SPAN(Apply);
            SWITCH_METAL_ENCODE_COUNT(Applied);
            SWITCH_METAL_ENCODE_COUNT(Applied, 2);
            spin(500);
        }
        SWITCH_METAL_ENCODE_LOCK(l, m);
    }
    gm20b::tEncodeProfile = nullptr;
    {
        SWITCH_METAL_ENCODE_LOCK(l, m);
        SWITCH_METAL_ENCODE_COUNT(Sends);
    }
    const auto self = [&](gm20b::EncodePart p) {
        return profile.self[size_t(p)];
    };
    const auto ticks = [&](gm20b::EncodePart p) {
        return profile.ticks[size_t(p)];
    };
    check(ticks(gm20b::EncodePart::Loop) >= ticks(gm20b::EncodePart::State) + 500);
    check(self(gm20b::EncodePart::Loop) >= 500 &&
          self(gm20b::EncodePart::Loop) < ticks(gm20b::EncodePart::Loop) - 1000);
    check(self(gm20b::EncodePart::State) >= 500 && self(gm20b::EncodePart::State) < ticks(gm20b::EncodePart::State));
    check(self(gm20b::EncodePart::Apply) == ticks(gm20b::EncodePart::Apply) && ticks(gm20b::EncodePart::Apply) >= 500);
    check(self(gm20b::EncodePart::Loop) + self(gm20b::EncodePart::State) + self(gm20b::EncodePart::Apply) ==
          ticks(gm20b::EncodePart::Loop));
    check(profile.counts[size_t(gm20b::EncodeCount::Applied)] == 3);
    check(profile.counts[size_t(gm20b::EncodeCount::Locks)] == 1);
    check(profile.counts[size_t(gm20b::EncodeCount::Sends)] == 0);
    check(profile.open == nullptr);

    gm20b::EncodeTotals totals;
    totals.add(profile, 1);
    check(totals.counts[size_t(gm20b::EncodeCount::Applied)].load() == 3);
    check(totals.self[size_t(gm20b::EncodePart::Apply)].load() == self(gm20b::EncodePart::Apply));
}
}  // namespace

int main()
{
    outsideAnEncode();
    nested();
    selfTimesAndEvents();
    printf("encode profile: %u checks passed\n", checks);
}
