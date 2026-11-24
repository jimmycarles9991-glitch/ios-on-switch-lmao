// <switch/workloop_handoff.h> and diag/workloop_line.h: which events of a kind a process logs and the SwitchWL handoff
// line tools/devicerun reads; that iOS 27's kern_event.c hooks each event where its knote, workloop and threads are
// still held, that iOS 17's has no hook, and that a release kernel compiles every hook to nothing.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_workloop_line.h"

using switch_ios::test::Between;
using switch_ios::test::Body;
using switch_ios::test::Configured;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

// tools/devicerun/dispatch_test.py reads these same lines.
constexpr const char *kWaitUnheld =
    "SwitchWL handoff wait-unheld #1: SwitchHWTest[412] thread 0x2f1c workloop 0x10471c0c0 ident 0x2a03 dq_state "
    "0x60000000002a03 fflags 0x4 flags 0x11 owner none servicer 0x2f20";
constexpr const char *kDropWake =
    "SwitchWL handoff drop-wake #16: locationd[73] thread 0x336 workloop 0xc1d0a4000 ident 0x4c03 dq_state 0x0 "
    "fflags 0x28 flags 0x6 owner other servicer self waiter 0x341";

std::string Format(enum switch_wl_kind kind, uint32_t n, const char *process, int pid, uint64_t thread,
                   const void *self, const switch_wl_event &event)
{
    char line[320];
    const int written = switch_wl_format(line, sizeof(line), kind, n, process, pid, thread, self, &event);
    EXPECT_GT(written, 0);
    EXPECT_LT(written, static_cast<int>(sizeof(line)));
    return line;
}

std::string Hooks(const std::string &source)
{
    return Configured(source, "XNU_12377_121_6", true);
}

}  // namespace

TEST(WorkloopHandoff, AProcessLogsItsFirstEventsOfAKindThenEachPowerOfTwo)
{
    for (uint32_t n = 1; n <= SWITCH_WL_FIRST; n++)
        EXPECT_TRUE(switch_wl_counted(n)) << n;
    for (uint32_t n : {9u, 10u, 15u, 17u, 31u, 33u, 1000u})
        EXPECT_FALSE(switch_wl_counted(n)) << n;
    for (uint32_t n : {16u, 32u, 64u, 1024u, 1u << 31})
        EXPECT_TRUE(switch_wl_counted(n)) << n;
}

TEST(WorkloopHandoff, AWaitReturningUnheldNamesTheOwnerAndServicerItRead)
{
    int threads[3];
    switch_wl_event event{};
    event.workloop = 0x10471c0c0;
    event.ident = 0x2a03;
    event.dq_state = 0x60000000002a03;
    event.fflags = 0x4;
    event.flags = 0x11;
    event.held = true;
    event.servicer = &threads[1];
    event.servicer_tid = 0x2f20;
    EXPECT_EQ(Format(SWITCH_WL_WAIT_UNHELD, 1, "SwitchHWTest", 412, 0x2f1c, &threads[0], event), kWaitUnheld);
}

TEST(WorkloopHandoff, ADropNamesTheWaiterItWokeAndOnlyComparesTheOthers)
{
    int threads[3];
    switch_wl_event event{};
    event.workloop = 0xc1d0a4000;
    event.ident = 0x4c03;
    event.fflags = 0x28;
    event.flags = 0x6;
    event.owner = &threads[2];
    event.owner_tid = 0x999;
    event.servicer = &threads[0];
    event.waiter = &threads[1];
    event.waiter_tid = 0x341;
    EXPECT_EQ(Format(SWITCH_WL_DROP_WAKE, 16, "locationd", 73, 0x336, &threads[0], event), kDropWake);
    event.owner = &threads[1];
    EXPECT_THAT(Format(SWITCH_WL_DROP_WAKE, 16, "locationd", 73, 0x336, &threads[0], event), HasSubstr("owner waiter"));
}

TEST(WorkloopHandoffSource, Xnu12377HooksEachEventWhereItsStateIsHeld)
{
    const std::string source = SourceText("../xnu-12377.121.6/bsd/kern/kern_event.c");
    const std::string wait = Body(source, "filt_wlwait_continue(void *parameter, wait_result_t wr)\n{");
    const std::string locked = Between(wait, "kqlock(kqwl);", "kqunlock(kqwl);");
    EXPECT_THAT(locked, HasSubstr("wr == THREAD_AWAKENED &&"));
    EXPECT_THAT(locked,
                HasSubstr("switch_diag_wl_hold(&switch_wl, kqwl->kqwl_owner, kqr_thread(&kqwl->kqwl_request))"));
    EXPECT_THAT(Between(wait, "turnstile_cleanup();", "kevent_register_wait_return(cont_args);"),
                HasSubstr("switch_diag_wl_event(SWITCH_WL_WAIT_UNHELD, &switch_wl);"));
    EXPECT_THAT(wait, Not(HasSubstr("kn->")));

    const std::string drop = Body(source, "filt_wlallow_drop(struct knote *kn, struct kevent_qos_s *kev)\n{");
    const std::string around = Between(drop, "switch_diag_wl_waking(", "assert(error != EPREEMPTDISABLED);");
    EXPECT_THAT(around, HasSubstr("== NOTE_WL_SYNC_WAIT, kn->kn_thread);\n\t\terror = filt_wlupdate(kqwl, kn, kev, 0, "
                                  "FILT_WLDROP);"));
    EXPECT_THAT(around, HasSubstr("if (switch_woken != THREAD_NULL && error == 0) {"));

    const std::string touch = Body(source, "filt_wltouch(struct knote *kn, struct kevent_qos_s *kev)\n{");
    EXPECT_THAT(Between(touch, "switch_diag_wl_touched(", "return kevent_register_wait_prepare"),
                HasSubstr("kn->kn_thread == THREAD_NULL &&\n\t    switch_diag_wl_unheld("));
    EXPECT_THAT(Between(touch, "out:\n\tif (error) {", "kev->flags |= EV_ERROR;"),
                HasSubstr("switch_wl_line(SWITCH_WL_OWNER_DEAD"));
    const std::string attach = Body(source, "filt_wlattach(struct knote *kn, struct kevent_qos_s *kev)\n{");
    EXPECT_THAT(Between(attach, "out:\n\tif (error) {", "knote_set_error(kn, error);"),
                HasSubstr("switch_wl_line(SWITCH_WL_OWNER_DEAD"));

    const std::string reg = Body(source, "kevent_register(struct kqueue *kq, struct kevent_qos_s *kev,");
    EXPECT_THAT(Between(reg, "\t\t\terror = ENOENT;\n", "\t\tgoto out;"),
                HasSubstr("switch_wl_line(SWITCH_WL_END_ENOENT"));
}

TEST(WorkloopHandoffSource, Xnu10063HasNoHandoffHook)
{
    const std::string source = SourceText("../xnu-10063.141.1/bsd/kern/kern_event.c");
    EXPECT_THAT(source, Not(HasSubstr("switch_wl_")));
    EXPECT_THAT(source, Not(HasSubstr("SWITCH_WL_")));
    const std::string logger = SourceText("../switch/diag/workloop.c");
    EXPECT_THAT(Configured(logger, "XNU_12377_121_6", false), Not(HasSubstr("switch_diag_wl_event(")));
    EXPECT_THAT(Hooks(logger), HasSubstr("void switch_diag_wl_event(enum switch_wl_kind kind"));
}

TEST(WorkloopHandoffSource, ReleaseKernelsCompileEveryHookToNothing)
{
    const std::string header = SourceText("../switch/include/switch/diag.h");
    const std::string release =
        Between(Between(header, "#if DEVELOPMENT || DEBUG\n", "\n__END_DECLS"), "\n#else\n", "\n#endif\n");
    EXPECT_THAT(release, HasSubstr("static inline void switch_diag_wl_event("));
    EXPECT_THAT(release, HasSubstr("static inline void switch_diag_wl_hold("));
    EXPECT_THAT(Between(release, "static inline bool switch_diag_wl_unheld(", "}"), HasSubstr("return false;"));
    EXPECT_THAT(Between(release, "static inline const void *switch_diag_wl_waking(", "}"), HasSubstr("return NULL;"));
    const std::string logger = SourceText("../switch/diag/workloop.c");
    EXPECT_THAT(Between(logger, "#if DEVELOPMENT || DEBUG\n", "void switch_diag_wl_event("),
                HasSubstr("#if XNU_12377_121_6\n"));
    const std::string helper =
        Body(SourceText("../xnu-12377.121.6/bsd/kern/kern_event.c"), "switch_wl_line(enum switch_wl_kind kind,");
    EXPECT_THAT(Between(helper, "#if DEVELOPMENT || DEBUG\n", "\n#else\n"), HasSubstr("switch_diag_wl_event("));
    EXPECT_THAT(Between(helper, "\n#else\n", "\n#endif\n"), Not(HasSubstr("switch_diag_wl_event")));
}
