// Guards on NativeSwitchBlock.cpp's use of SwitchSdPool.h (sd_pool_test.cpp tests the pool and lock protocol; the
// driver is not built for the host): single-lock pop+enqueue, zero IOMalloc under queueLock, unlocked completion
// teardown, and clean slab release on stop/free/setup failure.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Index;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &driver()
{
    static const std::string source = SourceText("../NativeSwitchBlock.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(driver(), signature);
}
}  // namespace

TEST(SdPoolSource, SlabPoolAndLockDiscipline)
{
    const std::string &source = driver();
    EXPECT_THAT(source, HasSubstr("#include \"SwitchSdPool.h\""));
    EXPECT_THAT(source, HasSubstr("constexpr uint32_t kRequestPool = 1024;"));
    EXPECT_THAT(source, HasSubstr("constexpr size_t kCompleteChunk = 16;"));
    EXPECT_THAT(source, HasSubstr("static_assert(sizeof(Request) <= 64,"));

    // doAsyncReadWrite never calls IOMalloc; it retains buffer, delegates to submit(), and releases on error.
    const std::string rw = body("IOReturn NativeSwitchBlockDevice::doAsyncReadWrite(");
    EXPECT_THAT(rw, Not(HasSubstr("IOMalloc")));
    EXPECT_THAT(rw, Not(HasSubstr("IOFree")));
    EXPECT_LT(Index(rw, "buffer->retain();"), Index(rw, "owner->submit(spec)"));
    EXPECT_THAT(rw, HasSubstr("buffer->release();"));

    // submit() delegates to switchSdPoolSubmit() so pool pop + enqueue share one queueLock hold and heap overflow
    // allocates outside queueLock with shutdownDone re-checked on re-acquire.
    const std::string submit = body("IOReturn NativeSwitchBlock::submit(");
    EXPECT_THAT(submit, HasSubstr("switchSdPoolSubmit(requestPool, spec,"));
    EXPECT_THAT(submit, HasSubstr("switch_refusal_k(kPoolRefusal,"));
    EXPECT_THAT(submit, HasSubstr("IOMalloc(sizeof(Request))"));
    EXPECT_THAT(submit, HasSubstr("IOFree(request, sizeof(*request))"));

    // completeBatch() clears b.first and delegates to switchSdPoolComplete() so pooled nodes return under queueLock
    // while IOFree, buffer->release(), and completion.action run outside queueLock.
    const std::string complete = body("void NativeSwitchBlock::completeBatch(");
    EXPECT_LT(Index(complete, "b.first = nullptr;"), Index(complete, "switchSdPoolComplete<kCompleteChunk,"));
    EXPECT_THAT(complete, HasSubstr("IOFree(r, sizeof(*r));"));
    EXPECT_THAT(complete, HasSubstr("d.buffer->release();"));
    EXPECT_THAT(complete, HasSubstr("d.completion.action("));
}

TEST(SdPoolSource, LifecycleAndTeardown)
{
    const std::string setup = body("bool NativeSwitchBlock::setup()");
    EXPECT_LT(Index(setup, "IOMallocZero(kRequestPool * sizeof(Request))"),
              Index(setup, "requestPool.init(requestSlab, kRequestPool)"));
    EXPECT_LT(Index(setup, "requestPool.init(requestSlab, kRequestPool)"), Index(setup, "kernel_thread_start("));
    EXPECT_LT(Index(setup, "kernel_thread_start("), Index(setup, "releaseRequestPool();"));

    const std::string release = body("void NativeSwitchBlock::releaseRequestPool()");
    EXPECT_LT(Index(release, "requestPool.detachIfIdle()"), Index(release, "IOLockUnlock(queueLock);"));
    EXPECT_LT(Index(release, "IOLockUnlock(queueLock);"),
              Index(release, "IOFree(slab, kRequestPool * sizeof(Request))"));

    const std::string stop = body("void NativeSwitchBlock::stop(");
    EXPECT_THAT(stop, HasSubstr("suspended = true;"));
    EXPECT_THAT(stop, HasSubstr("shutdownDone = true;"));
    EXPECT_THAT(stop, HasSubstr("drainQueueLocked()"));
    EXPECT_LT(Index(stop, "completeBatch(b);"), Index(stop, "releaseRequestPool();"));
    EXPECT_LT(Index(stop, "releaseRequestPool();"), Index(stop, "IOService::stop(provider);"));

    const std::string freeFn = body("void NativeSwitchBlock::free()");
    EXPECT_LT(Index(freeFn, "releaseRequestPool();"), Index(freeFn, "IOService::free();"));
}
