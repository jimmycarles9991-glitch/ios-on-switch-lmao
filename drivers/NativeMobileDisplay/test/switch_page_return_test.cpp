// SPDX-License-Identifier: BSD-2-Clause-Patent
// Core Animation's three IOMobileFramebuffer pages, each frame scanned in place (offered with a completion flip, or
// flipped from the kick once its producer was done) or copied into a scan buffer, against window A's gate, ring and
// completion flips (switch_completion_flip_harness.h): a page goes back to Core Animation when the swap after the one
// that showed it retires (SwitchRetireDisplay.h), and Core Animation renders into a page only once it is back. At every
// hand-back and every frame boundary the page must be out of window A's reach (not the address it scans, not one a
// request still out may latch, not an entry the ring still holds) and no copy may still read it; the debug kernels'
// check (SwitchScanReturn.h) must name the same page.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <sstream>
#include <string>

#include "../SwitchScanReturn.h"
#include "switch_completion_flip_harness.h"
#include "switch_completion_flip_walk.h"

namespace completion_flip_test {
namespace {

constexpr int kPages = 3;

struct Pages {
    enum State : uint8_t {
        kBack,
        kRendering,  // Core Animation's GPU work on it is not complete
        kSwapped,
    };

    std::array<State, kPages> state{kBack, kBack, kBack};
    std::array<uint64_t, kPages> swap{};
    std::array<bool, kPages> copyReads{};  // a GPU copy of it for window A is not done
    std::array<char, kPages> tags{};
    switch_display::ScanReturn scan;  // the debug kernels' view of the same hand-backs
    size_t retiredSeen = 0;
    int rendering = -1;
    unsigned handedBack = 0, replaced = 0;

    static int entry(int page)
    {
        return Display::kSlot0 + page;
    }
};

std::deque<std::string> gTrace;

void note(Display &d, const char *what)
{
    std::ostringstream o;
    o << what << ": ring f" << d.ring.front << " p" << d.ring.pending << " s" << d.ring.superseded << " h"
      << d.ring.held << " | active " << d.activeEntry() << " armed "
      << d.surfaceIndex(d.model.armed[Display::kWinbufStartAddr]) << " req " << d.model.requested << " | flip "
      << int(d.completionFlip.state()) << " e" << d.completionFlip.entry << " | swaps";
    for (const auto &s : d.swaps)
        o << " " << s.seq;
    o << " | retired " << (d.retired.empty() ? 0 : d.retired.back()) << " | seq";
    for (int i = Display::kSlot0; i < Display::kSlot0 + 3; ++i)
        o << " " << d.entrySeq[i];
    gTrace.push_back(o.str());
    if (gTrace.size() > 40)
        gTrace.pop_front();
}

std::string trace()
{
    std::string all;
    for (const auto &line : gTrace)
        all += line + "\n";
    return all;
}

// Whether window A may read the entry now or from the next boundary on.
std::string reach(Display &d, int entry)
{
    const uint32_t address = d.surfaceAddress(entry);
    if (d.hwSuspended) {
        if (d.model.assembly[Display::kWinbufStartAddr] == address)
            return "the resume gives window A it";
        return d.ring.busy(entry) ? "the ring still holds it" : "";
    }
    if (d.activeEntry() == entry)
        return "window A scans it";
    if ((d.model.requested & Display::kWinAActReq) && d.model.armed[Display::kWinbufStartAddr] == address)
        return "a request still out latches it";
    if (d.ring.busy(entry))
        return "the ring still holds it";
    return {};
}

void takeBack(Display &d, Pages &p)
{
    for (; p.retiredSeen < d.retired.size(); ++p.retiredSeen) {
        const uint64_t completed = d.retired[p.retiredSeen];
        const void *scanned = p.scan.retired(completed);
        const void *given = nullptr;
        for (int page = 0; page < kPages; ++page) {
            if (p.state[page] != Pages::kSwapped || p.swap[page] >= completed)
                continue;
            const std::string why = reach(d, Pages::entry(page));
            ASSERT_TRUE(why.empty()) << trace() << "swap " << completed << " retired gave back page " << page
                                     << " (swap " << p.swap[page] << ") while " << why;
            ASSERT_FALSE(p.copyReads[page]) << trace() << "swap " << completed << " gave back page " << page
                                            << " before its copy for window A was done";
            p.state[page] = Pages::kBack;
            given = &p.tags[page];
            ++p.handedBack;
        }
        ASSERT_EQ(scanned, given) << trace() << "the debug kernels' check and Core Animation disagree on what swap "
                                  << completed << " gave back";
    }
}

void render(Display &d, Pages &p)
{
    if (p.rendering >= 0)
        return;
    int pick = -1;
    for (int page = 0; page < kPages; ++page)
        if (p.state[page] == Pages::kBack && (pick < 0 || p.swap[page] < p.swap[pick]))
            pick = page;
    if (pick < 0)
        return;
    const std::string why = reach(d, Pages::entry(pick));
    ASSERT_TRUE(why.empty()) << trace() << "Core Animation renders into page " << pick << " while " << why;
    p.state[pick] = Pages::kRendering;
    p.rendering = pick;
}

void boundaryCheck(Display &d, const Pages &p)
{
    for (int page = 0; page < kPages; ++page)
        ASSERT_FALSE(p.state[page] == Pages::kRendering && d.activeEntry() == Pages::entry(page))
            << "window A latched page " << page << " while Core Animation renders into it";
}

class PageReturn : public testing::TestWithParam<uint64_t> {
protected:
    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
        gRng = GetParam();
    }

    void TearDown() override
    {
        gOffer = nullptr;
    }
};

// Offers only as the kick takes them (no frame held, no replaced flip unsettled, nothing out); the producer completes
// through its interrupt filter, its completion or both, in any order against boundaries and the frame-end action.
TEST_P(PageReturn, NoPageGoesBackWhileWindowACanStillScanIt)
{
    Display d;
    Pages p;
    bool offered = false, written = false, done = false, rendered = false, copying = false, superseded = false;
    unsigned pendingTicks = 0, visible = 0, copies = 0;
    for (unsigned step = 0; step < 60000; ++step) {
        switch (roll(11)) {
        case 0:
            ASSERT_NO_FATAL_FAILURE(render(d, p)) << "step " << step;
            rendered = false;
            note(d, "render");
            break;
        case 1:
        case 2: {
            const int page = p.rendering;
            if (page < 0 || offered || d.transferred || d.hwSuspended || d.ring.held != Display::kNoBuffer ||
                d.ring.superseded != Display::kNoBuffer || d.ring.busy(Pages::entry(page)))
                break;
            const bool copy = roll(4) == 0;
            if (copy) {
                // offerGpuCopy: direct scanout refused; the copy acquires the producer and reads the page
                if (d.offerCopy() != 0)
                    break;
                note(d, ("copy page " + std::to_string(page)).c_str());
                p.swap[page] = d.flipRequest;
                p.scan.taken(d.flipRequest, &p.tags[page]);
                p.copyReads[page] = true;
                offered = copying = true;
                written = done = false;
                if (rendered) {
                    written = true;
                    p.state[page] = Pages::kSwapped;
                    p.rendering = -1;
                }
                ++copies;
                break;
            }
            if (rendered) {
                // presentDirectVisible: the producer's writes were in DRAM at the kick, which flips from the gate
                d.flipRequest = ++d.nextSwap;
                d.swaps.push_back({d.flipRequest});
                d.presentEntry(Pages::entry(page));
                note(d, ("flip from the kick page " + std::to_string(page)).c_str());
                p.swap[page] = d.flipRequest;
                p.scan.taken(d.flipRequest, &p.tags[page]);
                p.state[page] = Pages::kSwapped;
                p.rendering = -1;
                ++visible;
                break;
            }
            ASSERT_EQ(d.offer(Pages::entry(page)), 0);
            note(d, ("offer page " + std::to_string(page)).c_str());
            p.swap[page] = d.flipRequest;
            p.scan.taken(d.flipRequest, &p.tags[page]);
            offered = true;
            copying = false;
            written = done = false;
            break;
        }
        case 10:
            if (p.rendering >= 0 && !offered)
                rendered = true;
            break;
        case 3:
        case 4:
            if (!offered || done)
                break;
            if (!written) {
                written = true;
                p.state[p.rendering] = Pages::kSwapped;
                p.rendering = -1;
                if (!copying && roll(2)) {
                    d.gpuFilter();
                    note(d, "filter");
                } else {
                    note(d, "producer done");
                }
                break;
            }
            d.complete(0);
            note(d, "complete");
            if (copying)
                for (bool &reads : p.copyReads)
                    reads = false;
            done = true;
            break;
        case 5:
            d.donePass();
            note(d, "pass");
            break;
        case 6:
            if (!d.hwSuspended) {
                d.boundary();
                note(d, "BOUNDARY");
                ++pendingTicks;
                ASSERT_NO_FATAL_FAILURE(boundaryCheck(d, p)) << "step " << step;
            }
            break;
        case 7:
        case 8:
            if (pendingTicks && !d.hwSuspended) {
                pendingTicks = 0;
                ASSERT_NO_THROW(d.tick()) << "step " << step;
                note(d, "tick");
            }
            break;
        case 9:
            if (d.hwSuspended ? roll(4) == 0 : roll(256) == 0) {
                if (d.hwSuspended) {
                    d.resume();
                    d.boundary();
                    note(d, "resume");
                } else {
                    ASSERT_NO_THROW(d.suspend()) << "step " << step;
                    note(d, "suspend");
                    pendingTicks = 0;
                }
            }
            break;
        }
        if (offered && done && !d.transferred)
            offered = false;
        if (d.ring.superseded != Display::kNoBuffer && !superseded)
            ++p.replaced;
        superseded = d.ring.superseded != Display::kNoBuffer;
        ASSERT_NO_FATAL_FAILURE(takeBack(d, p)) << "step " << step;
    }
    EXPECT_GT(p.handedBack, 2000u);
    EXPECT_GT(visible, 200u);
    EXPECT_GT(copies, 200u);
    EXPECT_GT(p.replaced, 100u);
}

INSTANTIATE_TEST_SUITE_P(Orders, PageReturn,
                         testing::Values(0x9e3779b97f4a7c15ull, 0x243f6a8885a308d3ull, 0x13198a2e03707344ull,
                                         0xa4093822299f31d0ull));

}  // namespace
}  // namespace completion_flip_test
