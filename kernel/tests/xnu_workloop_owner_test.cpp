// xnu-12377's filt_wlupdate (xnu_workloop_owner_harness.c) on the owner hint iOS 27's libdispatch sends from
// _dispatch_event_loop_wait_for_ownership: fflags 0x388, naming the thread its lock word holds. xnu-13432 lets a hint
// make that thread the owner of a workloop nobody owns or services, and otherwise drops only the reference the name
// lookup took. A hint never attaches a knote or wakes a waiter.
#include <gtest/gtest.h>

#include <cerrno>

#include "wl_flags.h"
#include "xnu_workloop_owner_harness.h"

namespace {

constexpr int kOwner = 0, kNamed = 1, kWaiter = 2, kServicer = 3;
constexpr int kAttach = 0, kTouch = 1, kDrop = 2;
constexpr uint16_t kAddDisable = 0x1 | 0x8, kDeleteEnable = 0x2 | 0x4;
constexpr uint32_t kHint = NOTE_WL_SYNC_WAKE | NOTE_WL_DISCOVER_OWNER | NOTE_WL_IGNORE_ESTALE | NOTE_WL_OWNER_HINT;

xnu_wl_kevent Hint(int named)
{
    return {.op = kAttach,
            .fflags = kHint,
            .flags = kAddDisable,
            .named = named,
            .stale = false,
            .self = kWaiter,
            .waiter = XNU_WL_NONE,
            .sfflags = kHint};
}

// Each thread holds its own reference and the workloop's owner one more.
void ExpectReferencesBalanced()
{
    for (int t = 0; t < XNU_WL_THREADS; t++)
        EXPECT_EQ(xnu_wl_refs(t), 1 + (xnu_wl_owner() == t)) << "thread " << t;
    EXPECT_TRUE(xnu_wl_locks_balanced());
}

}  // namespace

TEST(WorkloopOwnerHint, IsTheFflags24B119sLibdispatchSends)
{
    EXPECT_EQ(kHint, 0x388u);
}

TEST(WorkloopOwnerHint, NamingAnotherThreadLeavesTheOwnerItsReferenceAndOverride)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, true);
    const xnu_wl_kevent hint = Hint(kNamed);
    const xnu_wl_result r = xnu_wl_update(&hint);
    EXPECT_EQ(r.error, xnu_wl_no_attach());
    EXPECT_FALSE(r.woke);
    EXPECT_EQ(xnu_wl_owner(), kOwner);
    EXPECT_FALSE(xnu_wl_owner_hint());
    EXPECT_EQ(xnu_wl_overrides(kOwner), 1);
    EXPECT_EQ(xnu_wl_overrides(kNamed), 0);
    ExpectReferencesBalanced();
}

// The device's panic: a hint while the owner holds the workloop, then the owner's own SYNC_END deletes its woken
// SYNC_WAIT knote, and filt_wldetach drops the knote's reference on it.
TEST(WorkloopOwnerHint, TheOwnersEndAfterAHintDoesNotFreeIt)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, false);
    const xnu_wl_kevent hint = Hint(kNamed);
    xnu_wl_update(&hint);
    const xnu_wl_kevent end = {.op = kDrop,
                               .fflags = NOTE_WL_SYNC_WAKE | NOTE_WL_END_OWNERSHIP,
                               .flags = kDeleteEnable,
                               .named = XNU_WL_NONE,
                               .stale = false,
                               .self = kOwner,
                               .waiter = kOwner,
                               .sfflags = NOTE_WL_SYNC_WAIT | NOTE_WL_SYNC_WAKE};
    EXPECT_EQ(xnu_wl_update(&end).error, 0);
    EXPECT_EQ(xnu_wl_owner(), XNU_WL_NONE);
    EXPECT_TRUE(xnu_wl_detach(kOwner)) << "filt_wldetach freed a thread that was never terminated";
    ExpectReferencesBalanced();
}

TEST(WorkloopOwnerHint, ManyHintsWhileTheOwnerHoldsTheWorkloopCostItNothing)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, false);
    for (int i = 0; i < 8; i++) {
        const xnu_wl_kevent hint = Hint(i % 2 ? kNamed : kWaiter);
        EXPECT_EQ(xnu_wl_update(&hint).error, xnu_wl_no_attach());
    }
    EXPECT_EQ(xnu_wl_owner(), kOwner);
    ExpectReferencesBalanced();
}

TEST(WorkloopOwnerHint, IsIgnoredWhileAServicerRunsTheWorkloop)
{
    xnu_wl_reset(XNU_WL_NONE, kServicer, false);
    const xnu_wl_kevent hint = Hint(kNamed);
    EXPECT_EQ(xnu_wl_update(&hint).error, xnu_wl_no_attach());
    EXPECT_EQ(xnu_wl_owner(), XNU_WL_NONE);
    ExpectReferencesBalanced();
}

TEST(WorkloopOwnerHint, MakesTheNamedThreadTheOwnerOfAWorkloopNobodyRuns)
{
    xnu_wl_reset(XNU_WL_NONE, XNU_WL_NONE, false);
    const xnu_wl_kevent hint = Hint(kNamed);
    const xnu_wl_result r = xnu_wl_update(&hint);
    EXPECT_EQ(r.error, xnu_wl_no_attach());
    EXPECT_FALSE(r.woke);
    EXPECT_EQ(xnu_wl_owner(), kNamed);
    EXPECT_TRUE(xnu_wl_owner_hint());
    ExpectReferencesBalanced();
}

TEST(WorkloopOwnerHint, ThatIsStaleChangesNothing)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, false);
    xnu_wl_kevent hint = Hint(kNamed);
    hint.stale = true;
    EXPECT_EQ(xnu_wl_update(&hint).error, ESTALE);
    EXPECT_EQ(xnu_wl_owner(), kOwner);
    ExpectReferencesBalanced();
}

// A pre-woken SYNC_WAIT knote returns its waiter at once: it would run inside the queue beside the owner.
TEST(WorkloopOwnerHint, NeverWakesAWaiterOrMarksItsKnote)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, false);
    xnu_wl_kevent hint = Hint(kNamed);
    hint.op = kTouch;
    hint.waiter = kNamed;
    hint.sfflags = NOTE_WL_SYNC_WAIT;
    const xnu_wl_result r = xnu_wl_update(&hint);
    EXPECT_EQ(r.error, xnu_wl_no_attach());
    EXPECT_FALSE(r.woke);
    EXPECT_EQ(r.sfflags, static_cast<uint32_t>(NOTE_WL_SYNC_WAIT));
    EXPECT_EQ(xnu_wl_owner(), kOwner);
    EXPECT_TRUE(xnu_wl_detach(kNamed));
    ExpectReferencesBalanced();
}

TEST(WorkloopOwnerHandoff, AWakeMovesTheOwnersReferenceToTheWaiter)
{
    xnu_wl_reset(kOwner, XNU_WL_NONE, false);
    const xnu_wl_kevent wake = {.op = kTouch,
                                .fflags = NOTE_WL_SYNC_WAKE | NOTE_WL_DISCOVER_OWNER,
                                .flags = kAddDisable,
                                .named = kWaiter,
                                .stale = false,
                                .self = kOwner,
                                .waiter = kWaiter,
                                .sfflags = NOTE_WL_SYNC_WAIT};
    const xnu_wl_result r = xnu_wl_update(&wake);
    EXPECT_EQ(r.error, xnu_wl_preempt_disabled());
    EXPECT_TRUE(r.woke);
    EXPECT_EQ(xnu_wl_owner(), kWaiter);
    EXPECT_TRUE(xnu_wl_detach(kWaiter));
    ExpectReferencesBalanced();
}
