// SPDX-License-Identifier: MIT
// gm20b_cache.h and metallib.h on made-up records and a made-up universal metallib (harness/cachecheck runs the
// same cache checks on a bucket's real records, and the AIR hashes on QuartzCore's own library).
#include "../gm20b_cache.h"
#include "../metallib.h"
#include "../render_key.h"
#include "../harness/cache_checks.h"

#include <gtest/gtest.h>

#include <mach/mach.h>
#include <pthread/qos.h>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <thread>

namespace {

TEST(RenderCacheKey, PreservesFrameworkKeyAndFullPrimaryHashes)
{
    gm20b::AirHash vertex{}, fragment{};
    vertex[0] = 1;
    fragment[0] = 2;
    const auto key = gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false);
    EXPECT_EQ(key.key, "descriptor");
    EXPECT_EQ(key.stack, 0x53u);
    EXPECT_EQ(std::memcmp(key.vertexAir, vertex.data(), vertex.size()), 0);
    EXPECT_EQ(std::memcmp(key.fragmentAir, fragment.data(), fragment.size()), 0);
    vertex[31] = 7;
    EXPECT_NE(key.name(), gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false).name());
}

TEST(RenderCacheKey, PreservesOwnLibrarySuffix)
{
    gm20b::AirHash vertex{}, fragment{};
    for (unsigned i = 0; i < 32; ++i) {
        vertex[i] = i;
        fragment[i] = i + 32;
    }
    const auto key = gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, true);
    EXPECT_EQ(key.key, "descriptor | air 0706050403020100 2726252423222120");
    EXPECT_NE(key.name(), gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {}, false).name());
}

TEST(RenderCacheKey, LinkedStageAndDescriptorOrderRemainSignificant)
{
    gm20b::AirHash vertex{}, fragment{}, first{}, second{};
    first[0] = 1;
    second[0] = 2;
    const std::vector<gm20b::LinkedStageAir> linked{{'v', first}, {'f', second}};
    const auto key = gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, linked, false);
    EXPECT_EQ(key.key, "descriptor | linked v0000000000000001 f0000000000000002");
    EXPECT_NE(key.name(),
              gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {{'f', second}, {'v', first}}, false).name());
    EXPECT_NE(key.name(),
              gm20b::renderCacheKey(0x53, "descriptor", vertex, fragment, {{'f', first}, {'f', second}}, false).name());
}

TEST(RenderCacheKey, ConstructionDoesNotAppendSuffixesToTheInput)
{
    const std::string base = "descriptor";
    gm20b::AirHash vertex{}, fragment{};
    const auto first = gm20b::renderCacheKey(0x53, base, vertex, fragment, {{'f', vertex}}, true);
    const auto second = gm20b::renderCacheKey(0x53, base, vertex, fragment, {{'f', vertex}}, true);
    EXPECT_EQ(base, "descriptor");
    EXPECT_EQ(first.key, second.key);
    EXPECT_EQ(first.name(), second.name());
    EXPECT_NE(first.name(), gm20b::renderCacheKey(0x54, base, vertex, fragment, {{'f', vertex}}, true).name());
}

gm20b::RuntimeRecord madeUp(uint32_t n)
{
    gm20b::RuntimeRecord r;
    r.pipeline.keyHash = 0x1000 + n;
    r.pipeline.vs = 0;
    r.pipeline.fs = 1;
    gm20b::setEntryCount(r.pipeline, 2);
    r.pipeline.entry[0] = {0, gm20b::EntryUbo, 1, uint8_t(n), 0, 16, 0};
    r.pipeline.entry[1] = {16, gm20b::EntryTexture, 2, 3, gm20b::SamplerSlot, 3, 0};
    r.pipeline.pushMask = 1u << (n % 32);
    r.pipeline.pushWords[n % 32] = n;
    if (n % 2) {
        r.pipeline.flags |= gm20b::PipelinePushMetadata;
        r.pipeline.halfBlendColorWord = 29;
        r.pipeline.pushMask |= 0xf0000ff0u;
    }
    r.keyString = "samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 " + std::to_string(n);
    for (int k = 0; k < 2; k++) {
        r.programs[k].id = std::to_string(n * 2 + k);
        r.programs[k].image.assign(80 + 64 * (n + 1), uint8_t(n + k));
        r.programs[k].cbufTypes = {1, 3};
        r.programs[k].gprs = uint8_t(24 + 8 * k);
    }
    r.programs[1].boundTextures = 2;
    if (n % 2 == 0) {
        r.pipeline.variantCount[1] = 1;
        r.pipeline.uniform[0] = {0, 1, 0, 4};
        r.pipeline.fetchRung[0] = gm20b::FetchInterlock;
        gm20b::RuntimeRecord::Variant &v = r.variants.emplace_back();
        v.assume = gm20b::AssumeUniformAligned;
        v.program = r.programs[1];
        v.program.id += "-aligned";
        v.program.gprs = 16;
    }
    r.programs[1].data.assign(256, 0x5A);
    r.programs[1].cbufTypes = {1, 2, 3};
    r.blob = {0x1A00, n, 0x1E08, n + 1};
    if (n % 2 == 1) {
        r.pipeline.spillEntry[1] = 1;
        r.pipeline.spillWords[1] = 4;
        r.spill = {n, n + 1, n + 2, n + 3};
    }
    if (n % 3 == 0) {
        r.extras = {{0x1340, n}};
        r.restore = {{0x1340, 0}};
        r.samplers.push_back({n, 1, 2, 3, 4, 5, 6, 7});
    }
    r.splitTried = n % 2 == 0;
    if (n % 4 == 0) {
        r.split = true;
        r.splitRecord.thresholdPixels = 900 + n;
        r.splitRecord.frontierBytes = 48;
        r.splitRecord.frontierCbuf = 3;
        r.splitRecord.uboCount = 1;
        r.splitRecord.uboCbuf[0] = 1;
        r.splitRecord.prologueCbufs = 3;
        r.splitRecord.qmd[n % 64] = n;
        r.splitRecord.cost = {1, 0, 4000, 4000, 91 + n, 72 + n, 8 + n, 3 + n, 12};
        for (int k = 0; k < 2; k++) {
            r.splitPrograms[k].id = "split" + std::to_string(n * 2 + k);
            r.splitPrograms[k].image.assign(80 + 32 * (n + 1), uint8_t(n + 7 * k));
            r.splitPrograms[k].cbufTypes = {4, 1};
        }
    }
    if (n % 5 == 1) {
        r.pipeline.vertexSlot = 2;
        r.pipeline.vertexStrideDynamic = 1;
        auto twin = std::make_shared<gm20b::RuntimeRecord>(madeUp(n + 1));
        twin->pipeline.keyHash = 0x2000 + n;
        twin->pipeline.pulledStride[0][0] = 2;
        twin->pipeline.pulledStride[0][1] = 7;
        r.strideTwin = std::move(twin);
    }
    return r;
}

void tag(std::vector<uint8_t> &out, const char *fourcc, const void *data, uint16_t length)
{
    out.insert(out.end(), fourcc, fourcc + 4);
    out.push_back(uint8_t(length));
    out.push_back(uint8_t(length >> 8));
    const auto *b = static_cast<const uint8_t *>(data);
    out.insert(out.end(), b, b + length);
}

template <class T> void put(std::vector<uint8_t> &out, size_t at, T v)
{
    memcpy(&out[at], &v, sizeof v);
}

void bigEndian(std::vector<uint8_t> &out, size_t at, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        out[at + i] = uint8_t(v >> (24 - 8 * i));
}

// A universal file of an Apple GPU slice and an AIR64 slice whose function list names `functions`, each HASH a pattern of its
// index
std::vector<uint8_t> universalMetallib(const std::vector<std::string> &functions)
{
    std::vector<uint8_t> slice(0x58);
    memcpy(slice.data(), "MTLB", 4);
    std::vector<uint8_t> list(4);
    put(list, 0, uint32_t(functions.size()));
    for (size_t i = 0; i < functions.size(); i++) {
        std::vector<uint8_t> e(4);
        tag(e, "NAME", functions[i].c_str(), uint16_t(functions[i].size() + 1));
        const uint8_t type = 1;
        tag(e, "TYPE", &type, 1);
        gm20b::AirHash hash;
        hash.fill(uint8_t(0xA0 + i));
        tag(e, "HASH", hash.data(), uint16_t(hash.size()));
        e.insert(e.end(), {'E', 'N', 'D', 'T'});
        put(e, 0, uint32_t(e.size()));
        list.insert(list.end(), e.begin(), e.end());
    }
    put(slice, 24, uint64_t(slice.size()));
    put(slice, 32, uint64_t(list.size() - 4));
    slice.insert(slice.end(), list.begin(), list.end());
    std::vector<uint8_t> file(0x100, 0);
    bigEndian(file, 0, 0xCAFEBABE);
    bigEndian(file, 4, 2);
    bigEndian(file, 8, 0x01000013);  // an Apple GPU binary first, as the frameworks ship them
    bigEndian(file, 16, 0x100);
    bigEndian(file, 20, 16);
    bigEndian(file, 28, 0x01000017);
    bigEndian(file, 36, 0x110);
    bigEndian(file, 40, uint32_t(slice.size()));
    file.resize(0x110, 0xEE);
    file.insert(file.end(), slice.begin(), slice.end());
    return file;
}

TEST(GM20BMetallib, ReadsTheAirSlicesFunctionHashes)
{
    const std::vector<std::string> names = {"fixed_vert_lph_spc", "fixed_frag_lph_cph", "downsample_4_frag_lph"};
    const std::vector<uint8_t> file = universalMetallib(names);
    std::map<std::string, gm20b::AirHash> hashes;
    ASSERT_TRUE(gm20b::metallibAirHashes(file.data(), file.size(), hashes));
    ASSERT_EQ(hashes.size(), names.size());
    for (size_t i = 0; i < names.size(); i++) {
        gm20b::AirHash want;
        want.fill(uint8_t(0xA0 + i));
        EXPECT_EQ(hashes[names[i]], want) << names[i];
    }
    const std::string path = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/universal.metallib";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    std::map<std::string, gm20b::AirHash> fromFile;
    ASSERT_TRUE(gm20b::metallibAirHashes(path, fromFile));
    EXPECT_EQ(fromFile, hashes);
    std::vector<uint8_t> cut(file.begin(), file.end() - 10);
    EXPECT_FALSE(gm20b::metallibAirHashes(cut.data(), cut.size(), hashes));
    EXPECT_TRUE(hashes.empty());
}

TEST(GM20BCache, KeepsRecordsAcrossProcessesAndMakesEachStateOnce)
{
    std::vector<std::pair<std::string, gm20b::RuntimeRecord>> records;
    for (uint32_t n = 0; n < 12; n++)
        records.push_back(
            {"samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0  | v v" + std::to_string(n) + " | f f", madeUp(n)});
    cachechecks::run(records, "samples=1 | v two_draws | f f", "2 draws", getenv("TEST_TMPDIR") ?: "/tmp");
    EXPECT_EQ(cachechecks::failures, 0);
}

// A cached record is read where its draws index by its entries: one of an entry no draw can write comes back as none,
// so its state is made afresh rather than drawn from it
TEST(GM20BCache, ARecordOfAnEntryNoDrawCanWriteComesBackAsNone)
{
    gm20b::CacheKey k;
    k.stack = 0x51;
    k.key = "entries";
    gm20b::RuntimeRecord back;
    std::string refusal;
    const gm20b::RuntimeRecord intact = madeUp(2);
    EXPECT_EQ(gm20b::decodeEntry(gm20b::encodeEntry(k, &intact, ""), k, back, refusal),
              gm20b::RecordCache::Found::Record);
    gm20b::RuntimeRecord slot = madeUp(2), kind = madeUp(2), stage = madeUp(2), offset = madeUp(2);
    slot.pipeline.entry[0].slot = gm20b::kStageBufferArguments;
    kind.pipeline.entry[0].kind = uint8_t(gm20b::EntryTessBuffer + 1);
    stage.pipeline.entry[0].stage = 3;
    offset.pipeline.entry[0].offset = 256;
    for (const gm20b::RuntimeRecord *broken : {&slot, &kind, &stage, &offset})
        EXPECT_EQ(gm20b::decodeEntry(gm20b::encodeEntry(k, broken, ""), k, back, refusal),
                  gm20b::RecordCache::Found::None);
}

TEST(GM20BCache, KeepsKernelsAndTheirRefusals)
{
    const std::string dir = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/kernels";
    mkdir(dir.c_str(), 0700);
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.open(dir, 0x77, 1 << 20));
    gm20b::CacheKey made, refused;
    made.stack = refused.stack = 0x77;
    made.key = "kernel | 16ff | max=0 tgm=0 | tg=96,1,1 | tgmem= | tex= | smp=";
    refused.key = "kernel | 16ff | max=0 tgm=0 | tg=32,32,1 | tgmem= | tex= | smp=";
    gm20b::ComputeRecord r;
    r.kernel.keyHash = 0xABCD;
    r.kernel.regionWord = 4;
    r.kernel.entries = 2;
    r.kernel.entry[0] = {0, gm20b::KernelBuffer, 2, 0x3000040};
    r.kernel.entry[1] = {16, gm20b::KernelSampler, 0x80, 0};
    r.kernel.qmd[20] = 0x7;
    r.code.assign(320, 0x11);
    r.data.assign(256, 0x22);
    r.cbufType[0] = gm20b::CbufRoot;
    r.cbufType[1] = gm20b::CbufSet;
    r.cbufs = 2;
    r.keyString = made.key;
    r.samplers.push_back({1, 2, 3, 4, 5, 6, 7, 8});
    r.kernel.spillEntry = 1;
    r.kernel.spillWords = 2;
    r.spill = {0x51, 0x52};
    ASSERT_TRUE(cache.storeKernel(made, &r, ""));
    ASSERT_TRUE(cache.storeKernel(refused, nullptr, "a texture slot the dispatch leaves unbound at 0"));
    gm20b::ComputeRecord back;
    std::string why;
    ASSERT_EQ(cache.loadKernel(made, back, why), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(memcmp(&back.kernel, &r.kernel, sizeof r.kernel), 0);
    EXPECT_EQ(back.code, r.code);
    EXPECT_EQ(back.data, r.data);
    EXPECT_EQ(memcmp(back.cbufType, r.cbufType, sizeof r.cbufType), 0);
    EXPECT_EQ(back.cbufs, r.cbufs);
    EXPECT_EQ(back.keyString, r.keyString);
    EXPECT_EQ(back.samplers, r.samplers);
    EXPECT_EQ(back.spill, r.spill);
    EXPECT_EQ(cache.loadKernel(refused, back, why), gm20b::RecordCache::Found::Refusal);
    EXPECT_EQ(why, "a texture slot the dispatch leaves unbound at 0");
    gm20b::RuntimeRecord state;
    EXPECT_EQ(cache.load(made, state, why), gm20b::RecordCache::Found::None);
}

// Cold work is deliberately held at a barrier: unrelated warm work and callbacks must progress before it is released.
TEST(GM20BCompiler, IndependentColdKeysOverlapWhileWarmKeysRemainReady)
{
    std::mutex lock;
    std::condition_variable changed;
    unsigned entered = 0, active = 0, maximum = 0;
    bool release = false;
    std::atomic<unsigned> makes{0}, callbacks{0};
    const std::string dir = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/parallel-cache";
    mkdir(dir.c_str(), 0700);
    gm20b::RecordCache cache;
    ASSERT_TRUE(cache.open(dir, 0x98, 1 << 20));
    gm20b::CacheKey warm;
    warm.stack = 0x98;
    warm.key = "warm";
    auto original = madeUp(1);
    ASSERT_TRUE(cache.store(warm, &original, ""));
    gm20b::Compiler compiler(&cache, [&](const gm20b::CacheKey &, gm20b::RuntimeRecord &record, std::string &) {
        makes++;
        std::unique_lock<std::mutex> guard(lock);
        ++entered;
        maximum = std::max(maximum, ++active);
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        --active;
        record = madeUp(2);
        return gm20b::Compiler::Outcome::Made;
    });
    gm20b::CacheKey cold[3];
    for (unsigned i = 0; i < 3; ++i) {
        cold[i].stack = 0x98;
        cold[i].key = "cold-" + std::to_string(i);
    }
    for (unsigned i = 0; i < 2; ++i)
        EXPECT_EQ(compiler.ask(cold[i], nullptr), gm20b::Compiler::State::Pending);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered == 2;
        }));
    }
    // A warm lookup and an asynchronous warm ask both bypass the occupied compile slots.
    gm20b::RuntimeRecord loaded;
    std::string why;
    EXPECT_EQ(compiler.lookup(warm, loaded, why), gm20b::RecordCache::Found::Record);
    EXPECT_EQ(loaded.keyString, original.keyString);
    gm20b::CacheKey warmAsync = warm;
    warmAsync.key = "warm-async";
    EXPECT_TRUE(cache.store(warmAsync, &original, ""));
    std::atomic<bool> warmDone{false};
    compiler.ask(warmAsync, [&](std::shared_ptr<const gm20b::RuntimeRecord> record, const std::string &) {
        EXPECT_NE(record, nullptr);
        std::lock_guard<std::mutex> guard(lock);
        warmDone = true;
        changed.notify_all();
    });
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return warmDone.load();
        }));
    }
    // Repeated requests share the running key, and a third cold key cannot exceed the worker bound.
    for (unsigned i = 0; i < 32; ++i)
        compiler.ask(cold[0], [&](std::shared_ptr<const gm20b::RuntimeRecord> record, const std::string &) {
            EXPECT_NE(record, nullptr);
            callbacks++;
        });
    compiler.ask(cold[2], nullptr);
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
        changed.notify_all();
    }
    compiler.drain();
    EXPECT_EQ(makes.load(), 3u);
    EXPECT_EQ(callbacks.load(), 32u);
    EXPECT_EQ(maximum, 2u);
    // Drain includes background persistence, not just callback publication.
    gm20b::RecordCache reopened;
    ASSERT_TRUE(reopened.open(dir, 0x98, 1 << 20));
    for (const auto &key : cold)
        EXPECT_EQ(reopened.load(key, loaded, why), gm20b::RecordCache::Found::Record);
}

TEST(GM20BCompiler, ComputeTasksShareTheRenderWorkerLimitAndDrain)
{
    std::mutex lock;
    std::condition_variable changed;
    bool release = false;
    unsigned entered = 0, active = 0, maximum = 0;
    auto work = [&] {
        std::unique_lock<std::mutex> guard(lock);
        maximum = std::max(maximum, ++active);
        ++entered;
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        --active;
    };
    gm20b::Compiler compiler(nullptr, [&](const gm20b::CacheKey &, gm20b::RuntimeRecord &record, std::string &) {
        work();
        record = madeUp(1);
        return gm20b::Compiler::Outcome::Made;
    });
    compiler.enqueue(work);
    compiler.enqueue(work);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered == 2;
        }));
    }
    gm20b::CacheKey key;
    key.key = "render-after-compute";
    compiler.ask(key, nullptr);
    compiler.enqueue(work);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_FALSE(changed.wait_for(guard, std::chrono::milliseconds(50), [&] {
            return entered > 2;
        }));
        release = true;
        changed.notify_all();
    }
    compiler.drain();
    EXPECT_EQ(entered, 4u);
    EXPECT_EQ(active, 0u);
    EXPECT_EQ(maximum, 2u);
}

int scheduledPriority()
{
    thread_extended_info_data_t info = {};
    mach_msg_type_number_t count = THREAD_EXTENDED_INFO_COUNT;
    const mach_port_t self = mach_thread_self();
    const kern_return_t kr = thread_info(self, THREAD_EXTENDED_INFO, reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), self);
    return kr == KERN_SUCCESS ? info.pth_curpri : -1;
}

// Two utility-class compiles hold both slots; a user-interactive creation that waits for one raises them both until a
// slot is free, and then makes its own state.
TEST(GM20BCompiler, AWaitingCreationLendsItsClassToTheSlotHolders)
{
    std::mutex lock;
    std::condition_variable changed;
    unsigned holding = 0, raised = 0;
    std::atomic<bool> waiterMade{false};
    gm20b::Compiler compiler(nullptr, [&](const gm20b::CacheKey &k, gm20b::RuntimeRecord &record, std::string &) {
        record = madeUp(3);
        if (k.key == "waiter")
            return gm20b::Compiler::Outcome::Made;
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        const int own = scheduledPriority();
        {
            std::lock_guard<std::mutex> guard(lock);
            ++holding;
            changed.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (scheduledPriority() <= own && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::unique_lock<std::mutex> guard(lock);
        raised += scheduledPriority() > own;
        changed.notify_all();
        // the first to give its slot back ends the lending: both hold theirs until both were raised
        changed.wait_until(guard, deadline, [&] {
            return raised == 2;
        });
        return gm20b::Compiler::Outcome::Made;
    });
    gm20b::CacheKey held[2], waiter;
    for (unsigned i = 0; i < 2; ++i)
        held[i].key = "held-" + std::to_string(i);
    waiter.key = "waiter";
    for (const auto &k : held)
        compiler.ask(k, nullptr);
    {
        std::unique_lock<std::mutex> guard(lock);
        ASSERT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return holding == 2;
        }));
    }
    std::thread creation([&] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        gm20b::RuntimeRecord record;
        std::string why;
        waiterMade = compiler.makeNow(waiter, record, why) == gm20b::Compiler::State::Ready;
    });
    creation.join();
    compiler.drain();
    EXPECT_TRUE(waiterMade.load());
    EXPECT_EQ(raised, 2u);
}

// A state's creation while an ask of it is being made waits for that make, lending it its class, and takes its record:
// it is never handed back unready, and the state is made once
TEST(GM20BCompiler, ACreationWaitsForTheMakeAnAskStartedAndLendsItItsClass)
{
    std::mutex lock;
    std::condition_variable changed;
    bool making = false, raised = false;
    std::atomic<unsigned> makes{0};
    gm20b::Compiler compiler(nullptr, [&](const gm20b::CacheKey &, gm20b::RuntimeRecord &record, std::string &) {
        makes++;
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        const int own = scheduledPriority();
        {
            std::lock_guard<std::mutex> guard(lock);
            making = true;
            changed.notify_all();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (scheduledPriority() <= own && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        raised = scheduledPriority() > own;
        record = madeUp(4);
        return gm20b::Compiler::Outcome::Made;
    });
    gm20b::CacheKey k;
    k.key = "asked-then-created";
    EXPECT_EQ(compiler.ask(k, nullptr), gm20b::Compiler::State::Pending);
    {
        std::unique_lock<std::mutex> guard(lock);
        ASSERT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return making;
        }));
    }
    gm20b::Compiler::State created = gm20b::Compiler::State::Pending;
    gm20b::RuntimeRecord record;
    std::thread creation([&] {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        std::string why;
        created = compiler.makeNow(k, record, why);
    });
    creation.join();
    compiler.drain();
    EXPECT_EQ(created, gm20b::Compiler::State::Ready);
    EXPECT_EQ(record.pipeline.keyHash, madeUp(4).pipeline.keyHash);
    EXPECT_TRUE(raised);
    EXPECT_EQ(makes.load(), 1u);
}

// A creation on a fixed-priority thread with no class (Core Animation's render thread) compiles while both slots are held.
TEST(GM20BCompiler, AFixedPriorityCreationDoesNotWaitForASlot)
{
    std::mutex lock;
    std::condition_variable changed;
    unsigned holding = 0;
    bool release = false;
    gm20b::Compiler compiler(nullptr, [&](const gm20b::CacheKey &k, gm20b::RuntimeRecord &record, std::string &) {
        record = madeUp(4);
        if (k.key == "fixed")
            return gm20b::Compiler::Outcome::Made;
        std::unique_lock<std::mutex> guard(lock);
        ++holding;
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        return gm20b::Compiler::Outcome::Made;
    });
    gm20b::CacheKey held[2], fixed;
    for (unsigned i = 0; i < 2; ++i)
        held[i].key = "held-" + std::to_string(i);
    fixed.key = "fixed";
    for (const auto &k : held)
        compiler.ask(k, nullptr);
    {
        std::unique_lock<std::mutex> guard(lock);
        ASSERT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return holding == 2;
        }));
    }
    std::atomic<bool> fixedPriority{false}, made{false};
    std::thread creation([&] {
        sched_param param = {};
        param.sched_priority = 47;
        fixedPriority =
            pthread_setschedparam(pthread_self(), SCHED_RR, &param) == 0 && qos_class_self() == QOS_CLASS_UNSPECIFIED;
        if (!fixedPriority)
            return;
        gm20b::RuntimeRecord record;
        std::string why;
        made = compiler.makeNow(fixed, record, why) == gm20b::Compiler::State::Ready;
    });
    creation.join();
    EXPECT_TRUE(fixedPriority.load());
    EXPECT_TRUE(made.load());
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
        changed.notify_all();
    }
    compiler.drain();
}

TEST(GM20BCompiler, TrimFreesSettledEntriesAndKeepsHeldRecordsAndActiveMakersAlive)
{
    std::mutex lock;
    std::condition_variable changed;
    bool inMake = false, release = false;
    gm20b::Compiler compiler(nullptr, [&](const gm20b::CacheKey &k, gm20b::RuntimeRecord &record, std::string &) {
        record = madeUp(k.key == "inflight" ? 7 : 5);
        if (k.key == "inflight") {
            std::unique_lock<std::mutex> guard(lock);
            inMake = true;
            changed.notify_all();
            changed.wait(guard, [&] {
                return release;
            });
        }
        return gm20b::Compiler::Outcome::Made;
    });
    gm20b::CacheKey settled, inflight;
    settled.key = "settled";
    inflight.key = "inflight";
    gm20b::RuntimeRecord out;
    std::string why;
    ASSERT_EQ(compiler.makeNow(settled, out, why), gm20b::Compiler::State::Ready);
    std::shared_ptr<const gm20b::RuntimeRecord> held;
    ASSERT_EQ(compiler.ask(settled, nullptr, &held), gm20b::Compiler::State::Ready);
    ASSERT_NE(held, nullptr);

    ASSERT_EQ(compiler.ask(inflight, nullptr), gm20b::Compiler::State::Pending);
    {
        std::unique_lock<std::mutex> guard(lock);
        ASSERT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return inMake;
        }));
    }
    // Trim frees the settled entry while leaving the in-flight entry intact and keeping `held` valid.
    EXPECT_EQ(compiler.trim(), 1u);
    EXPECT_EQ(held->pipeline.keyHash, madeUp(5).pipeline.keyHash);
    EXPECT_EQ(compiler.lookup(settled, out, why), gm20b::RecordCache::Found::None);
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
        changed.notify_all();
    }
    compiler.drain();
    std::shared_ptr<const gm20b::RuntimeRecord> made;
    ASSERT_EQ(compiler.ask(inflight, nullptr, &made), gm20b::Compiler::State::Ready);
    ASSERT_NE(made, nullptr);
    EXPECT_EQ(made->pipeline.keyHash, madeUp(7).pipeline.keyHash);
    EXPECT_EQ(compiler.trim(), 1u);
}

}  // namespace
