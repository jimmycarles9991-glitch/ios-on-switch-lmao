// SPDX-License-Identifier: BSD-2-Clause-Patent
// A job without events reserves its destination for writing once it is admitted, when no other job touches the
// destination (SwitchVICRequests::alone, decided under the queue's lock), else when the engine thread runs it; a quarter
// turn recorded instead of copied is made under that reservation. The queue model in switch_vic_requests_test shows
// why a reservation held by a later job stalls an earlier one.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

TEST(TegraVICReservation, AJobReservesItsDestinationAfterAdmissionOnlyWhenNoOtherJobTouchesIt)
{
    const std::string source = SourceText("../TegraVICUserClient.cpp");
    EXPECT_THAT(Body(source, "IOReturn transform(Conn *conn, const void *in, uint32_t size)\n"),
                Not(HasSubstr("switch_vic_surface_reserve_write")));
    const std::string queue = Body(source, "bool queueJob(Job *job)\n");
    const size_t admitted = queue.find("!gRequests.admits(*job)");
    const size_t decided = queue.find("reserve = reserves && gRequests.alone(job->dst.memory);");
    const size_t unlocked = queue.find("IOLockUnlock(gQueueLock);\n    if (reserve) {");
    const size_t reserved = queue.find("switch_vic_surface_reserve_write(&job->dst, &job->dstReserve);");
    ASSERT_NE(admitted, std::string::npos);
    ASSERT_NE(decided, std::string::npos);
    ASSERT_NE(unlocked, std::string::npos);
    ASSERT_NE(reserved, std::string::npos);
    EXPECT_LT(admitted, decided);
    EXPECT_LT(decided, unlocked);
    EXPECT_LT(unlocked, reserved);
    EXPECT_THAT(queue, HasSubstr("gRequests.reserved(job);"));
}

TEST(TegraVICReservation, AJobThatCouldNotReserveWhenQueuedReservesWhenRunAndARecordedTurnUsesIt)
{
    const std::string source = SourceText("../TegraVICUserClient.cpp");
    const std::string run = Body(source, "bool runJob(Job *job, IOReturn *status)\n");
    EXPECT_THAT(run, HasSubstr("if (job->reserveWhenRun)\n        switch_vic_surface_reserve_write(&job->dst, "
                               "&dstReserve.reservation);"));
    EXPECT_THAT(run, HasSubstr("traits, &dstReserve.reservation))"));
    const std::string engine = Body(source, "void engineThread(void *, wait_result_t)\n");
    EXPECT_LT(engine.find("runJob(job, &status)"), engine.find("gRequests.finished();"));
}
