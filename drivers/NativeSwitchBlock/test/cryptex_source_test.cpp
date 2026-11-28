// SPDX-License-Identifier: BSD-2-Clause-Patent
// Guards on SwitchCryptex.cpp (cryptex_map_test.cpp tests the extent map it uses; the driver is not built for the
// host). The cryptex images are read straight from the SD card into requesting pages (direct read map) with a steady
// cache for small reads; boot prefetch lives in kernel/switch/vm/prefetch.c.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>

#include "bazel/testing/source_text.h"
#include "source_checks.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::Search;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {
const std::string &cryptex()
{
    static const std::string source = SourceText("../SwitchCryptex.cpp");
    return source;
}

std::string body(const char *signature)
{
    return Body(cryptex(), signature);
}
}  // namespace

TEST(CryptexSource, ReadsGoToTheCardWithoutACopy)
{
    const std::string read = body("IOReturn SwitchCryptexDevice::doAsyncReadWrite(");
    EXPECT_THAT(read,
                HasSubstr("if (cacheHitInline(buffer, block, count, completion, &p))\n        return kIOReturnSuccess;"));
    EXPECT_LT(read.find("cacheHitInline("), read.find("takePending()"));
    EXPECT_THAT(read,
                HasSubstr("if (cacheRead(p, buffer, block, count, cachePriority))\n        return kIOReturnSuccess;"));
    EXPECT_THAT(read, HasSubstr("switchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece)"));
    EXPECT_THAT(read, HasSubstr("if (done != 0 || piece.length != length)"));
    EXPECT_THAT(read, HasSubstr("IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn)"));
    EXPECT_THAT(read, HasSubstr("sd->doAsyncReadWrite(target, piece.disk / kSdSector, piece.length / kSdSector,"));
    for (const char *copy : {"readBytes", "memcpy", "bcopy", "copypv", "VNOP_READ", "vn_rdwr"})
        EXPECT_THAT(read, Not(HasSubstr(copy)));
    EXPECT_EQ(Count(read, "writeBytes("), 1u);
    EXPECT_THAT(read, HasSubstr("buffer->writeBytes(done + k, gZeros, kImageBlock)"));
    EXPECT_THAT(read, HasSubstr("return kIOReturnNotWritable;"));
    const std::string hit = body("bool SwitchCryptexDevice::cacheHitInline(");
    EXPECT_THAT(hit, HasSubstr("gCache.cache.tryHit("));
    EXPECT_THAT(hit, Not(HasSubstr("takePending")));
    EXPECT_THAT(hit, Not(HasSubstr("thread_call_enter")));
    EXPECT_LT(hit.find("IOLockUnlock(gCache.lock);"), hit.find("completion->action("));
    const std::string done = body("void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)");
    EXPECT_THAT(done, HasSubstr("thread_call_enter(p->call);"));
    EXPECT_THAT(done, Not(HasSubstr("client.action")));
    const std::string finish = body("void finishRequest(thread_call_param_t parameter, thread_call_param_t)");
    EXPECT_THAT(finish, HasSubstr("client.action(client.target, client.parameter, status, bytes);"));
}

TEST(CryptexSource, SmallReadsGoThroughTheCache)
{
    const std::string plan = body("bool SwitchCryptexDevice::cacheRead(");
    EXPECT_THAT(plan, HasSubstr("count * kImageBlock > kAroundBytes ? SwitchCache::kCoveredOnly : "
                                "SwitchCache::kReadAround"));
    EXPECT_LT(plan.find("p->cached = true;"), plan.find("gCache.cache.plan("));
    EXPECT_LT(plan.find("gCache.cache.plan("), plan.find("p->remaining = p->cache.waiting ? 2 : 1;"));
    EXPECT_LT(plan.find("p->remaining = p->cache.waiting ? 2 : 1;"), plan.find("IOLockUnlock(gCache.lock);"));
    EXPECT_THAT(plan, HasSubstr("buffer->retain();"));
    const std::string issue = body("uint32_t SwitchCryptexDevice::issueRead(");
    EXPECT_THAT(issue, HasSubstr("if (read.first != 0 || done != 0 || piece.length != length)"));
    EXPECT_THAT(issue, HasSubstr("IOSubMemoryDescriptor::withSubRange(memory,"));
    EXPECT_THAT(issue, HasSubstr("IOStorageCompletion pieceCompletion = {this, jobPieceDone, j};"));
    EXPECT_THAT(issue, HasSubstr("attrs.priority = priority;"));
    EXPECT_THAT(issue, HasSubstr("piece.length / kSdSector, &attrs, &pieceCompletion)"));
    EXPECT_THAT(issue, Not(HasSubstr("attrs.priority = kIOStoragePriorityHigh")));  // issued at its requester's
    EXPECT_THAT(issue, HasSubstr("bzero(bytes + done, piece.length);"));
    for (const char *copy : {"copypv", "memcpy", "bcopy", "writeBytes", "readBytes"})
        EXPECT_THAT(issue, Not(HasSubstr(copy)));
    const std::string finish = body("void finishRequest(thread_call_param_t parameter, thread_call_param_t)");
    EXPECT_LT(finish.find("copyOut(p)"), finish.find("gCache.cache.release(&p->cache, copied);"));
    EXPECT_LT(finish.find("gCache.cache.release(&p->cache, copied);"), finish.find("pushPendingLocked(p);"));
    EXPECT_LT(finish.find("pushPendingLocked(p);"), finish.find("IOLockUnlock(gCache.lock);"));
    EXPECT_LT(finish.find("IOLockUnlock(gCache.lock);"), finish.find("buffer->release();"));
    EXPECT_LT(finish.find("buffer->release();"), finish.find("client.action("));
    const std::string copy = body("bool copyOutRange(");
    EXPECT_THAT(copy, HasSubstr("switchClientCopy(buffer, bytes,"));
    EXPECT_THAT(copy, HasSubstr("copypv,"));
    const std::string job = body("void jobFinish(Job *j)");
    EXPECT_THAT(job, HasSubstr("gCache.cache.finished(j->window, j->first, j->count, j->status == kIOReturnSuccess)"));
    EXPECT_LT(job.find("pushJobLocked(j);"), job.find("IOLockUnlock(gCache.lock);"));
    EXPECT_LT(job.find("IOLockUnlock(gCache.lock);"), job.find("pieceDone(nullptr, ready->owner,"));
    EXPECT_THAT(job, Not(HasSubstr("client.action")));
    EXPECT_THAT(cryptex(), Not(HasSubstr("gPoolLock")));
}

// A window fill reads at High for a tier-0 requester and Default for any other, the boot replay included; a tier-0
// requester that joins it raises it through the SD device's doSetPriority, outside gCache.lock (the SD queueLock is
// never taken under it). XNU's own busy-page reprioritisation reaches the same code through this device's
// doSetPriority.
TEST(CryptexSource, JoinedFillsAreRaised)
{
    const std::string read = body("IOReturn SwitchCryptexDevice::doAsyncReadWrite(");
    EXPECT_THAT(read, HasSubstr("switch_vm_prefetch_is_replay_thread(current_thread())"));
    EXPECT_THAT(read, HasSubstr("switchSdClassOf(static_cast<uint8_t>(attributes->priority >> 6), replay) == 0"));
    EXPECT_LT(read.find("switchSdClassOf("), read.find("cacheRead(p, buffer, block, count, cachePriority)"));
    const std::string plan = body("bool SwitchCryptexDevice::cacheRead(");
    EXPECT_LT(plan.find("joined = p->cache.joined;"), plan.find("IOLockUnlock(gCache.lock);"));
    EXPECT_LT(plan.find("IOLockUnlock(gCache.lock);"), plan.find("raiseQueuedReads(tag,"));
    EXPECT_THAT(plan, HasSubstr("if (joined && priority == kIOStoragePriorityHigh) {"));
    EXPECT_LT(plan.find("raiseQueuedReads(tag,"), plan.find("pieceDone(this, p,"));
    EXPECT_THAT(plan, HasSubstr("for (uint32_t i = 0; i < p->cache.windows; ++i)"));  // review N7: whole windows
    const std::string raise = body("void SwitchCryptexDevice::raiseQueuedReads(");
    EXPECT_THAT(raise, HasSubstr("switch_block_raise(pieces, n, priority);"));
    EXPECT_THAT(raise, Not(HasSubstr("gCache.lock")));
    const std::string set = body("IOReturn SwitchCryptexDevice::doSetPriority(");
    EXPECT_LT(set.find("return kIOReturnBadArgument;"), set.find("raiseQueuedReads("));
    EXPECT_THAT(plan, HasSubstr("&n, priority == kIOStoragePriorityHigh);"));
}

// Review S3: a raise that comes between a fill's plan and its queuing finds nothing in the SD queue. The raiser marks
// the window (a tier-0 joiner in plan, XNU's doSetPriority here) before it promotes; the issuer reads the mark after
// its pieces are queued and before its issuing reference goes, and raises its own blocks.
TEST(CryptexSource, AFillPlannedButNotYetQueuedIsRaisedByItsIssuer)
{
    const std::string set = body("IOReturn SwitchCryptexDevice::doSetPriority(");
    EXPECT_LT(set.find("gCache.cache.markUrgent("), set.find("raiseQueuedReads("));
    EXPECT_LT(set.find("IOLockLock(gCache.lock);"), set.find("gCache.cache.markUrgent("));
    EXPECT_LT(set.find("gCache.cache.markUrgent("), set.find("IOLockUnlock(gCache.lock);"));
    const std::string issue = body("uint32_t SwitchCryptexDevice::issueRead(");
    const size_t queued = issue.rfind("sd->doAsyncReadWrite(");
    const size_t checked = issue.find("__atomic_load_n(&read.window->urgent, __ATOMIC_ACQUIRE)");
    const size_t released = issue.rfind("jobPieceDone(this, j, kIOReturnSuccess, 0);");
    ASSERT_NE(checked, std::string::npos);
    EXPECT_LT(queued, checked);
    EXPECT_LT(checked, released);
    EXPECT_THAT(issue,
                HasSubstr("raiseQueuedReads(read.window->tag + read.first, read.count, kIOStoragePriorityHigh);"));
    EXPECT_EQ(Count(cryptex(), "issueRead(p->reads[i], p->jobs[i], priority)"), 1u) << "cacheRead is its only caller";
    EXPECT_EQ(Count(cryptex(), "issueRead("), 3u) << "the declaration, the definition and cacheRead";
}

TEST(CryptexSource, MapIsCheckedAndFallsBackToDiskImages)
{
    const std::string resolve = body("Extent *resolveExtents(");
    EXPECT_THAT(resolve, HasSubstr("vnode_specrdev(devvp) != preboot"));
    EXPECT_THAT(resolve, HasSubstr("VNOP_BLOCKMAP(image,"));
    EXPECT_THAT(resolve, HasSubstr("DKIOCGETPHYSICALEXTENT"));
    EXPECT_THAT(resolve, HasSubstr("request.dev != whole"));
    EXPECT_THAT(resolve, HasSubstr("switchExtentsCover(map, n, bytes)"));
    const std::string attach = body("IOService *attachDirect(unsigned index, dev_t preboot, const char **why)");
    EXPECT_THAT(attach, HasSubstr("UF_COMPRESSED"));
    EXPECT_LT(attach.find("setupCache()"), attach.find("device->registerService();"));
    const std::string setup = body("bool setupCache()");
    EXPECT_THAT(setup, HasSubstr("kSteadyWindows"));
    EXPECT_THAT(setup, Not(HasSubstr("kBootWindows")));
    const std::string entry = body("extern \"C\" int switch_cryptex_attach(");
    EXPECT_THAT(entry, HasSubstr("direct read map refused"));
    EXPECT_THAT(entry, HasSubstr("attachDiskImages(index)"));
    const std::string mount = SourceText("../../../diagnostics/switch/SwitchRawMount.h");
    EXPECT_THAT(mount, HasSubstr("switch_cryptex_fallback(i, &image_volume, path, sizeof(path))"));
    EXPECT_THAT(mount, HasSubstr("switch_cryptex_mounted(i, cryptex_mount);"));
}

TEST(CryptexSource, PrefetchLivesOutsideTheCryptexDriver)
{
    for (const char *removed : {"advisory_read", "switch_block_prefetch_thread", "readAhead", "readBlock",
                                "vnode_pagein", "switch_cryptex_mounted", "switch_boot_io_complete", "kBootWindows"}) {
        EXPECT_THAT(cryptex(), Not(HasSubstr(removed)));
    }
}

TEST(CryptexSource, ACompletionRunsInTheKernelBand)
{
    EXPECT_THAT(cryptex(), Not(HasSubstr("THREAD_CALL_PRIORITY_KERNEL_HIGH")));
    EXPECT_THAT(
        body("Pending *takePending()"),
        HasSubstr("p->call = thread_call_allocate_with_options(finishRequest, p, THREAD_CALL_PRIORITY_KERNEL, 0);"));
}
