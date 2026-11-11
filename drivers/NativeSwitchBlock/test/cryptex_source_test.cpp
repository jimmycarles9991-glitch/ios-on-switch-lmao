// r17-launch: guards on SwitchCryptex.cpp (cryptex_map_test.cpp tests the map it uses; the driver is not built for the
// host). The cryptex images are read straight from the SD card into the requesting pages (the direct read map) instead
// of through DiskImages and the image files' page cache, and the boot prefetch pages the cryptex volumes' files in.
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
    // A read the cache does not take: each piece a sub-range of the request's own memory, issued on the SD device.
    EXPECT_THAT(read, HasSubstr("if (cacheRead(p, buffer, block, count))\n        return kIOReturnSuccess;"));
    EXPECT_THAT(read, HasSubstr("switchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece)"));
    EXPECT_THAT(read, HasSubstr("IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn)"));
    EXPECT_THAT(read, HasSubstr("sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, issue,"));
    for (const char *copy : {"readBytes", "memcpy", "bcopy", "copypv", "VNOP_READ", "vn_rdwr"})
        EXPECT_THAT(read, Not(HasSubstr(copy)));
    // The only bytes the CPU writes into a request here: zeros where a sparse image file stores nothing.
    EXPECT_EQ(Count(read, "writeBytes("), 1u);
    EXPECT_THAT(read, HasSubstr("buffer->writeBytes(done + k, gZeros, kImageBlock)"));
    EXPECT_THAT(read, HasSubstr("return kIOReturnNotWritable;"));
    // The prefetch threads' card reads take the SD driver's high class.
    EXPECT_THAT(read, HasSubstr("high.priority = kIOStoragePriorityHigh;"));
    EXPECT_THAT(read, HasSubstr("IOStorageAttributes *issue = p->prefetch ? &high : attributes;"));
    // Completions reach the client from a thread call, never from the SD driver's worker.
    const std::string done = body("void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)");
    EXPECT_THAT(done, HasSubstr("thread_call_enter(p->call);"));
    EXPECT_THAT(done, Not(HasSubstr("client.action")));
    const std::string finish = body("void finishRequest(thread_call_param_t parameter, thread_call_param_t)");
    EXPECT_THAT(finish, HasSubstr("client.action(client.target, client.parameter, status, bytes);"));
}

// Reads through the cache of windows (SwitchCryptexCache.h): the windows filled by the SD driver straight
// into their own memory, a request's blocks copied out on the thread call (copypv, no flush), never on the SD worker,
// and only while the request holds its windows.
TEST(CryptexSource, SmallReadsGoThroughTheCache)
{
    const std::string plan = body("bool SwitchCryptexDevice::cacheRead(");
    EXPECT_THAT(plan, HasSubstr("count * kImageBlock > kAroundBytes ? SwitchCache::kCoveredOnly : "
                                "SwitchCache::kReadAround"));
    // What finishRequest needs is set before the plan (a read under way can make the request ready at the unlock),
    // and the request's count of what it waits for under the cache's lock.
    EXPECT_LT(plan.find("p->cached = true;"), plan.find("gCache.cache.plan("));
    EXPECT_LT(plan.find("gCache.cache.plan("), plan.find("p->remaining = p->cache.waiting ? 2 : 1;"));
    EXPECT_LT(plan.find("p->remaining = p->cache.waiting ? 2 : 1;"), plan.find("IOLockUnlock(gCache.lock);"));
    EXPECT_THAT(plan, HasSubstr("buffer->retain();"));
    const std::string issue = body("uint32_t SwitchCryptexDevice::issueRead(");
    EXPECT_THAT(issue, HasSubstr("IOSubMemoryDescriptor::withSubRange(\n            memory,"));
    EXPECT_THAT(issue, HasSubstr("IOStorageCompletion pieceCompletion = {this, jobPieceDone, j};"));
    // Every cache read in the SD driver's high class, whichever request started it: a foreground read may wait on it.
    EXPECT_THAT(issue, HasSubstr("high.priority = kIOStoragePriorityHigh;"));
    EXPECT_THAT(issue, HasSubstr("piece.length / kSdSector, &high, &pieceCompletion)"));
    EXPECT_THAT(issue, HasSubstr("bzero(bytes + done, piece.length);"));
    for (const char *copy : {"copypv", "memcpy", "writeBytes"})
        EXPECT_THAT(issue, Not(HasSubstr(copy)));
    // The copy: on the thread call, from held windows, without readBytes/writeBytes' flush.
    const std::string copy = body("bool copyOut(const Pending *p)");
    EXPECT_THAT(copy, HasSubstr("switchClientCopy(p->buffer, bytes,"));
    EXPECT_THAT(copy, HasSubstr("copypv"));
    const std::string finish = body("void finishRequest(thread_call_param_t parameter, thread_call_param_t)");
    EXPECT_LT(finish.find("copyOut(p)"), finish.find("gCache.cache.release(&p->cache, copied);"));
    EXPECT_LT(finish.find("gCache.cache.release(&p->cache, copied);"), finish.find("client.action("));
    for (const char *worker : {"void jobFinish(Job *j)", "void jobPieceDone(void *, void *parameter, IOReturn status, "
                                                         "UInt64)"}) {
        EXPECT_THAT(body(worker), Not(HasSubstr("copyOut")));
        EXPECT_THAT(body(worker), Not(HasSubstr("client.action")));
    }
    EXPECT_THAT(body("void jobFinish(Job *j)"), HasSubstr("gCache.cache.finished(j->window, j->first, j->count,"));
}

// The cryptex APFS must mount without MNTK_VIRTUALDEV (clustered, cluster-layer-verified page-ins): the device reports
// SD storage, never the virtual interconnect DiskImages reports.
TEST(CryptexSource, NotAVirtualDeviceAndReadOnly)
{
    const std::string &source = cryptex();
    const std::string configure = body("bool SwitchCryptexDevice::configure(");
    EXPECT_THAT(configure, HasSubstr("putString(protocol, kIOPropertyPhysicalInterconnectTypeKey, \"SD\")"));
    EXPECT_THAT(configure, Not(HasSubstr("Virtual")));
    EXPECT_THAT(source, Not(HasSubstr("kIOPropertyPhysicalInterconnectTypeVirtual")));
    EXPECT_TRUE(Search(source, R"(reportWriteProtection\(bool \*value\) override\s*\{\s*\*value = true;)"));
}

// Extents resolved once, on the SD APFS window only, covering the image; sampled blocks compared both ways; a refused
// map attaches through DiskImages with one line; a failed mount falls back the same way.
TEST(CryptexSource, MapCheckedBeforeUse)
{
    const std::string resolve = body("Extent *resolveExtents(");
    EXPECT_THAT(resolve, HasSubstr("VNOP_BLOCKMAP(image,"));
    EXPECT_THAT(resolve, HasSubstr("DKIOCGETPHYSICALEXTENT"));
    EXPECT_THAT(resolve, HasSubstr("request.dev != whole"));
    EXPECT_THAT(resolve, HasSubstr("switchAppendExtent(map, &n, kMaxExtents, at, kSwitchHole, run)"));
    EXPECT_THAT(resolve, HasSubstr("switchExtentsCover(map, n, bytes)"));
    const std::string attach = body("IOService *attachDirect(unsigned index, dev_t preboot, const char **why)");
    EXPECT_THAT(attach, HasSubstr("UF_COMPRESSED"));
    EXPECT_THAT(attach, HasSubstr("sameBothWays(image, map[0], 0, sd, direct, file)"));
    EXPECT_THAT(attach, HasSubstr("sameBothWays(image, last, va.va_data_size - kImageBlock, sd, direct, file)"));
    // The cache is checked through the first direct device before it is registered, and turned off with one
    // line if a block read through it is not the image file's.
    EXPECT_THAT(attach, HasSubstr("proveCache(device, image, map, count, va.va_data_size)"));
    EXPECT_THAT(attach, HasSubstr("read cache off (%s); every read goes to the card"));
    EXPECT_LT(attach.find("proveCache("), attach.find("device->registerService();"));
    const std::string prove = body("const char *proveCache(");
    EXPECT_THAT(prove, HasSubstr("vn_rdwr(UIO_READ, image,"));
    EXPECT_THAT(prove, HasSubstr("device->readBlock(buffer, offsets[i] / kImageBlock)"));
    EXPECT_THAT(prove, HasSubstr("memcmp(buffer->getBytesNoCopy(), file, kImageBlock)"));
    EXPECT_THAT(body("IOReturn SwitchCryptexDevice::readBlock("), HasSubstr("panic("));
    const std::string entry = body("extern \"C\" int switch_cryptex_attach(");
    EXPECT_THAT(entry, HasSubstr("direct read map refused"));
    EXPECT_THAT(entry, HasSubstr("attachDiskImages(index)"));
    const std::string mount = SourceText("../../../diagnostics/switch/SwitchRawMount.h");
    EXPECT_THAT(mount, HasSubstr("switch_cryptex_fallback(i, &image_volume, path, sizeof(path))"));
    EXPECT_THAT(mount, HasSubstr("switch_cryptex_mounted(i, cryptex_mount);"));
}

// The prefetch replays page-ins of the cryptex files (absent pages, the files' own pages), bounded in flight; the old
// image-block replay through advisory reads of the image file is gone. kWorkers threads, each reading its
// run's absent pages ahead into the cache through a map checked on a real page first; a read that never finishes is
// a one-line panic.
TEST(CryptexSource, PrefetchPagesFilesIn)
{
    const std::string thread = body("void prefetchWorker(void *parameter, wait_result_t)");
    EXPECT_THAT(thread, HasSubstr("vnode_pagein(vp, nullptr, 0,"));
    EXPECT_THAT(thread, HasSubstr("waitFlowBelow(kInFlightBytes);"));
    EXPECT_LT(thread.find("readAheadRun(vp, run.index, offset, length);"), thread.find("vnode_pagein(vp, nullptr, 0,"));
    const std::string wait = body("void waitFlowBelow(uint64_t limit)");
    EXPECT_THAT(wait, HasSubstr("IOLockSleepDeadline(gFlow.lock, &gFlow.inflight, deadline, THREAD_UNINT)"));
    EXPECT_THAT(wait, HasSubstr("panic("));
    EXPECT_THAT(cryptex(), Not(HasSubstr("advisory_read")));
    EXPECT_THAT(cryptex(), Not(HasSubstr("switch_block_prefetch_thread")));
    EXPECT_THAT(thread, HasSubstr("VFS_VGET(gPrefetch.mounts[run.index]"));
    const std::string ahead = body("void readAheadRun(vnode_t vp, unsigned index, off_t offset, off_t length)");
    EXPECT_THAT(ahead, HasSubstr("ubc_range_op(vp, at, end, UPL_ROP_ABSENT, &absent)"));
    const std::string map = body("void mapAhead(");
    EXPECT_THAT(map, HasSubstr("VNOP_BLOCKMAP(vp,"));
    EXPECT_LT(map.find("mapProven(device, vp, index, at + done, image)"),
              map.find("device->readAhead(image, length);"));
    const std::string proven = body("bool mapProven(");
    EXPECT_THAT(proven, HasSubstr("vn_rdwr(UIO_READ, vp,"));
    EXPECT_THAT(proven, HasSubstr("device->readBlock(buffer, image / kPage)"));
    EXPECT_THAT(proven, HasSubstr("no read-ahead for %s (%s)"));
    EXPECT_THAT(body("int nameVnode(vnode_t vp, void *argument)"), HasSubstr("hit->vid != vnode_vid(vp)"));
    EXPECT_THAT(body("uint32_t buildRuns("), HasSubstr("vnode_iterate(mount, 0, nameVnode, &naming)"));
}

// A list this build will not replay makes the boot a recording boot with no prefetch: loadList checks it
// with SwitchCryptexMap.h's switchListRefusal (the format frozen by cryptex_map_test.cpp) and the boot says why.
TEST(CryptexSource, ARefusedListSaysWhy)
{
    const std::string load = body("Run *loadList(const ListHeader &key, uint32_t *count, const char **why)");
    EXPECT_THAT(load, HasSubstr("switchListRefusal(header, key.images, kLogEntries * 4u)"));
    EXPECT_THAT(load, HasSubstr("switchFnv(reinterpret_cast<const uint8_t *>(runs), bytes) != header.checksum"));
    EXPECT_THAT(body("void prefetchStart()"), HasSubstr("no list to replay (%s); this boot records one"));
    EXPECT_THAT(body("bool imageKeys(ListHeader *key)"), HasSubstr("key->version = kSwitchListVersion;"));
}

// AppleDiskImages2 completes an in-kernel image's requests on kernel_thread_start threads (81), and APFS hashes the
// sealed volume's blocks in the completion: kernel-high would put that above every IOWorkLoop.
TEST(CryptexSource, ACompletionRunsInTheKernelBand)
{
    EXPECT_THAT(cryptex(), Not(HasSubstr("THREAD_CALL_PRIORITY_KERNEL_HIGH")));
    EXPECT_THAT(
        body("Pending *takePending()"),
        HasSubstr("p->call = thread_call_allocate_with_options(finishRequest, p, THREAD_CALL_PRIORITY_KERNEL, 0);"));
}
