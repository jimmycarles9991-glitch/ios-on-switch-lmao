// SPDX-License-Identifier: MIT
// A command buffer that counts as scheduled before its work reaches the kernel (held on a shared event,
// raw_scheduled_wait.h) promises the surfaces it writes first; its last raw range's last job keeps the promises, and
// finish gives back what that range did not carry. No host model runs the facade, so these guard where RawMetal makes,
// carries and gives back the promises.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

void expectBefore(const std::string &text, const std::string &first, const std::string &second)
{
    const size_t a = text.find(first), b = text.find(second);
    ASSERT_NE(a, std::string::npos) << first;
    ASSERT_NE(b, std::string::npos) << second;
    EXPECT_LT(a, b) << first << " before " << second;
}

TEST(RawWriterPromiseSource, AHeldCommandBufferPromisesItsWritesBeforeItsWaitReturns)
{
    const std::string schedule = SourceText("../raw/runtime/RawMetalSchedule.mm");
    // the submit worker takes a commit (applyChange): one held at once promises its writes before it is marked held
    const std::string commit = Between(schedule, "static void applyChange(", "\n}\n");
    expectBefore(commit, "r.waiting.commit(", "promiseWrites(cb, writtenSurfaces(cb));");
    expectBefore(commit, "promiseWrites(cb, writtenSurfaces(cb));", "cb->_held = true;");
    expectBefore(commit, "cb->_held = true;", "rmChanged(cb);");
    const std::string mark = Between(schedule, "static void markHeld(", "\n}\n");
    expectBefore(mark, "promiseWrites(cb, writtenSurfaces(cb));", "cb->_held = held;");
    const std::string promise = Between(schedule, "static void promiseWrites(", "\n}\n");
    expectBefore(promise, "gRM->raw->promise(part, id)", "late = cb->_promisesTaken;");
    EXPECT_THAT(promise, HasSubstr("gRM->raw->cancelPromise(id);"));  // its last range went meanwhile: given back
    EXPECT_THAT(promise, HasSubstr("at += TegraGM20BMaxPromiseUses"));
    const std::string written = Between(schedule, "static std::vector<uint64_t> writtenSurfaces(", "\n}\n");
    EXPECT_THAT(written, HasSubstr("texture = texture->_parent;"));
}

TEST(RawWriterPromiseSource, TheLastRangeCarriesThemAndFinishGivesBackTheRest)
{
    const std::string encode = SourceText("../raw/runtime/RawMetalEncode.mm");
    expectBefore(encode, "rc->promises = cb->_promises;", "if (!rc->commit(seq)) {");
    expectBefore(encode, "if (rc->promisesKept) {", "cb->_promises.clear();");
    const std::string finish = SourceText("../raw/runtime/RawMetalFinish.mm");
    expectBefore(Between(finish, "static void finished(RMCommandBuffer *cb)\n{", "\n}\n"), "cancelPromises(cb);",
                 "unhold(cb);");
    const std::string events = SourceText("../raw/runtime/RawMetalEvents.mm");
    EXPECT_THAT(Between(events, "void unhold(RMCommandBuffer *cb)\n{", "\n}\n"), Not(HasSubstr("Promise")));
    const std::string gpu = SourceText("../raw/runtime/raw_gpu.cpp");
    EXPECT_THAT(gpu, HasSubstr("last && !promises.empty() ? &promises : nullptr"));
    expectBefore(gpu, "published = true;", "promisesKept |= last && !promises.empty();");
}

TEST(RawWriterPromiseSource, TheKernelCallsAreMethod34)
{
    const std::string kernel = SourceText("../raw/runtime/raw_kernel_job.cpp");
    const std::string make = Between(kernel, "int Kernel::promise(", "\n}\n");
    EXPECT_THAT(make, HasSubstr("TegraGM20BPromiseMake"));
    EXPECT_THAT(make, HasSubstr("call(TegraGM20BMethodWriterPromise,"));
    EXPECT_THAT(make, HasSubstr("uses.push_back({handle, TegraGM20BSurfaceWrite, 0});"));
    EXPECT_THAT(Between(kernel, "int Kernel::cancelPromise(", "\n}\n"), HasSubstr("TegraGM20BPromiseCancel"));
    const std::string submit = Between(kernel, "bool Kernel::submitJob(", "\n}\n");
    EXPECT_THAT(submit, HasSubstr("TegraGM20BJobFulfills"));
}

}  // namespace
