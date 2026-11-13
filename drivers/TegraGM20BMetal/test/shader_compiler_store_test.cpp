// SPDX-License-Identifier: MIT
// The shader compiler's store (compiler/compiler_store.h) and request log (compiler_log.h): a store per stack and pack
// rules, read whole by other processes and threads while the compiler writes it, whole after a writer killed mid-put, and
// a full one keeping its newest entries; the log keeps whole requests across a torn tail and only those a replay keeps.
#include "../compiler/compiler_log.h"
#include "../compiler/compiler_store.h"
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_compute_slot.h"
#include "../raw/runtime/raw_program_pages.h"
#include "../raw/runtime/raw_record_parts.h"

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>
#include <vector>

namespace {

namespace pack = switch_seed_pack;

constexpr uint64_t kStack = 0x5357495443480004ull;

raw::Digest digest(uint8_t fill, uint32_t n = 0)
{
    raw::Digest d;
    d.fill(fill);
    memcpy(d.data(), &n, sizeof n);
    return d;
}

raw::CacheKey keyOf(uint32_t n)
{
    raw::CacheKey k;
    k.stack = kStack;
    k.key = "samples=1 topo=3 ds=0/0 | v vertex_" + std::to_string(n) + " | f fragment";
    return k;
}

std::string entryOf(uint32_t n)
{
    raw::RuntimeRecord record;
    record.keyString = keyOf(n).key;
    record.programs[0].id = std::to_string(n);
    record.programs[1].id = std::string(n % 900, char('a' + n % 26));
    return raw::encodeEntry(keyOf(n), &record, "");
}

bool whole(const std::string &bytes, uint32_t n)
{
    raw::RuntimeRecord record;
    std::string refusal;
    return raw::decodeEntry(bytes, keyOf(n), record, refusal) == raw::RecordCache::Found::Record &&
           record.programs[0].id == std::to_string(n);
}

// A store's file mapped read only, as a GPU client maps the kernel's view of it
struct Client {
    pack::Bytes mapped;
    pack::StoreReader reader;

    explicit Client(const std::string &path)
    {
        bool absent = false;
        EXPECT_TRUE(pack::mapFile(path, mapped, absent));
        reader.attach(mapped.data, mapped.size, nullptr);
    }

    ~Client()
    {
        pack::unmap(mapped);
    }

    bool find(uint32_t n, std::string &bytes) const
    {
        pack::Bytes found;
        if (!reader.find(digest(0x51, n).data(), found))
            return false;
        bytes.assign(reinterpret_cast<const char *>(found.data), found.size);
        return true;
    }
};

class SharedStore : public testing::Test {
protected:
    std::filesystem::path root;

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/shared-store-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }

    std::string pathOf(uint64_t stack, const raw::Digest &rules) const
    {
        return (root / switch_compiler::storeDirectory(stack, rules) / "store").string();
    }

    void open(switch_compiler::Store &store, uint64_t stack = kStack, const raw::Digest &rules = digest(0x77))
    {
        std::string error;
        ASSERT_TRUE(store.open(root.string(), stack, rules, error)) << error;
    }
};

TEST_F(SharedStore, AStoreIsKeyedByStackAndPackRulesAndAnOldOneStaysWholeForItsReaders)
{
    const raw::Digest rules = digest(0x77), otherRules = digest(0x78);
    {
        switch_compiler::Store store;
        open(store);
        ASSERT_EQ(store.put(digest(0x51, 1), entryOf(1)), switch_compiler::Store::Put::Added);
    }
    Client old(pathOf(kStack, rules));
    std::string bytes;
    ASSERT_TRUE(old.find(1, bytes));
    switch_compiler::Store store;
    open(store, kStack, otherRules);
    EXPECT_FALSE(std::filesystem::exists(root / switch_compiler::storeDirectory(kStack, rules)));
    EXPECT_FALSE(store.find(digest(0x51, 1), bytes));
    EXPECT_TRUE(old.find(1, bytes) && whole(bytes, 1));
    switch_compiler::Store next;
    open(next, kStack + 1, otherRules);
    EXPECT_NE(next.path(), store.path());
}

// A made record's program slots of a page or more land on pages of their own, found by their bytes' name, ahead of the
// record: what a client runs in place from its mapping
TEST_F(SharedStore, ARecordsLargeSlotsArePublishedOnPagesOfTheirOwn)
{
    raw::RuntimeRecord record;
    record.keyString = keyOf(7).key;
    record.programs[0].id = "small";
    record.programs[0].image.assign(80 + 512, 0x11);
    record.programs[1].id = "large";
    record.programs[1].image.assign(80 + 200 * 1024, 0x22);
    record.programs[1].data.assign(512, 0x33);
    const std::string entry = raw::encodeEntry(keyOf(7), &record, "");
    switch_compiler::Store store;
    open(store);
    ASSERT_EQ(store.putProgramSlots(keyOf(7), entry), switch_compiler::Store::Put::Added);
    EXPECT_EQ(store.entries(), 1u) << "the small slot is copied by its client";
    EXPECT_EQ(store.putProgramSlots(keyOf(7), entry), switch_compiler::Store::Put::Added);
    EXPECT_EQ(store.entries(), 1u) << "a slot is kept once";
    ASSERT_EQ(store.put(digest(0x51, 7), entry), switch_compiler::Store::Put::Added);
    raw::PackProgram info;
    const std::vector<uint8_t> slot = raw::programSlot(record.programs[1], info);
    const raw::Digest name = raw::programSlotName(slot.data(), slot.size());
    Client client(store.path());
    pack::Bytes found;
    ASSERT_TRUE(client.reader.find(name.data(), found));
    EXPECT_EQ(uint64_t(found.data - client.mapped.data) % raw::kProgramSlotPage, 0u);
    EXPECT_EQ(found.size, raw::programSlotEntryBytes(slot.size()));
    EXPECT_EQ(memcmp(found.data, slot.data(), slot.size()), 0);
    for (size_t i = slot.size(); i < found.size; i++)
        ASSERT_EQ(found.data[i], 0) << "zeros through the prefetch past its end";
    std::string bytes, refusal;
    raw::RuntimeRecord read;
    ASSERT_TRUE(client.find(7, bytes));
    EXPECT_EQ(raw::decodeEntry(bytes, keyOf(7), read, refusal), raw::RecordCache::Found::Record);

    raw::ComputeRecord kernel;
    kernel.code.assign(48 * 1024, 0x44);
    kernel.data.assign(256, 0x55);
    EXPECT_EQ(store.putProgramSlots(keyOf(8), raw::encodeKernelEntry(keyOf(8), &kernel, "")),
              switch_compiler::Store::Put::Added);
    EXPECT_EQ(store.entries(), 3u) << "a kernel's slot as raw_compute.cpp places it";
    const std::vector<uint8_t> kernelSlot = raw::computeSlot(kernel);
    const raw::Digest kernelName = raw::programSlotName(kernelSlot.data(), kernelSlot.size());
    Client again(store.path());
    ASSERT_TRUE(again.reader.find(kernelName.data(), found));
    EXPECT_EQ(uint64_t(found.data - again.mapped.data) % raw::kProgramSlotPage, 0u);
}

TEST_F(SharedStore, ReadersInOtherThreadsAndProcessesOnlyEverSeeWholeEntries)
{
    switch_compiler::Store store;
    open(store);
    const std::string path = store.path();
    constexpr uint32_t kEntries = 3000;
    std::vector<pid_t> children;
    for (int c = 0; c < 2; ++c) {
        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (!child) {
            Client client(path);
            std::string bytes;
            for (int round = 0; round < 50; ++round)
                for (uint32_t n = 0; n < kEntries; ++n)
                    if (client.find(n, bytes) && !whole(bytes, n))
                        _exit(3);
            _exit(0);
        }
        children.push_back(child);
    }
    std::atomic<bool> writing{true};
    std::atomic<uint32_t> torn{0};
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t)
        readers.emplace_back([&] {
            Client client(path);
            std::string bytes;
            while (writing.load())
                for (uint32_t n = 0; n < kEntries; n += 7)
                    if (client.find(n, bytes) && !whole(bytes, n))
                        torn++;
        });
    for (uint32_t n = 0; n < kEntries; ++n)
        ASSERT_EQ(store.put(digest(0x51, n), entryOf(n)), switch_compiler::Store::Put::Added);
    writing = false;
    for (std::thread &t : readers)
        t.join();
    EXPECT_EQ(torn.load(), 0u);
    for (pid_t child : children) {
        int status = 0;
        waitpid(child, &status, 0);
        EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    }
    Client after(path);
    std::string bytes;
    for (uint32_t n = 0; n < kEntries; ++n)
        EXPECT_TRUE(after.find(n, bytes) && whole(bytes, n)) << n;
}

TEST_F(SharedStore, AWriterKilledMidPutLeavesEveryEntryAReaderFindsWhole)
{
    for (uint32_t round = 0; round < 6; ++round) {
        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (!child) {
            switch_compiler::Store store;
            std::string error;
            if (!store.open(root.string(), kStack, digest(0x77), error))
                _exit(2);
            for (uint32_t n = round * 1000; n < round * 1000 + 1000; ++n)
                store.put(digest(0x51, n), entryOf(n));
            _exit(0);
        }
        usleep(300 + round * 700);
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        switch_compiler::Store store;
        open(store);
        Client client(store.path());
        std::string bytes;
        for (uint32_t n = 0; n < (round + 1) * 1000; ++n)
            if (client.find(n, bytes))
                EXPECT_TRUE(whole(bytes, n)) << "round " << round << " entry " << n;
    }
}

TEST_F(SharedStore, AFullStoreKeepsItsNewestEntriesAndTakesTheNext)
{
    switch_compiler::Store store;
    open(store);
    const std::string big(1u << 20, 'x');
    uint32_t added = 0;
    while (store.put(digest(0x52, added), big + std::to_string(added)) == switch_compiler::Store::Put::Added)
        added++;
    ASSERT_GT(added, 100u);
    Client before(store.path());
    std::string error;
    ASSERT_TRUE(store.makeRoom(error)) << error;
    EXPECT_EQ(store.put(digest(0x52, added), big + std::to_string(added)), switch_compiler::Store::Put::Added);
    std::string bytes;
    EXPECT_FALSE(store.find(digest(0x52, 0), bytes));
    ASSERT_TRUE(store.find(digest(0x52, added - 1), bytes));
    EXPECT_EQ(bytes, big + std::to_string(added - 1));
    EXPECT_LE(store.entries(), added / 2 + 2);
    pack::Bytes found;
    EXPECT_TRUE(before.reader.find(digest(0x52, 0).data(), found));
}

// A slot the full store had no room for says so, so the compiler makes room and puts it with its record
TEST_F(SharedStore, ASlotAFullStoreRefusesIsPutAfterItsRoomIsMade)
{
    switch_compiler::Store store;
    open(store);
    const std::string big(1u << 20, 'x');
    uint32_t added = 0;
    while (store.put(digest(0x53, added), big + std::to_string(added)) == switch_compiler::Store::Put::Added)
        added++;
    raw::RuntimeRecord record;
    record.keyString = keyOf(9).key;
    record.programs[0].image.assign(80 + 512, 0x11);
    record.programs[1].image.assign(80 + 2048 * 1024, 0x22);
    const std::string entry = raw::encodeEntry(keyOf(9), &record, "");
    EXPECT_EQ(store.putProgramSlots(keyOf(9), entry), switch_compiler::Store::Put::Full);
    std::string error;
    ASSERT_TRUE(store.makeRoom(error)) << error;
    EXPECT_EQ(store.putProgramSlots(keyOf(9), entry), switch_compiler::Store::Put::Added);
    raw::PackProgram info;
    const std::vector<uint8_t> slot = raw::programSlot(record.programs[1], info);
    Client client(store.path());
    pack::Bytes found;
    EXPECT_TRUE(client.reader.find(raw::programSlotName(slot.data(), slot.size()).data(), found));
}

TEST_F(SharedStore, TheLogKeepsWholeRequestsAcrossATornTail)
{
    const std::string path = (root / "requests").string();
    {
        switch_compiler::RequestLog log;
        ASSERT_TRUE(log.open(path));
        for (uint8_t i = 0; i < 3; ++i)
            log.add(digest(0x60 + i), "request " + std::to_string(i));
        log.add(digest(0x60), "request 0 again");
        log.add(digest(0x70), std::string(switch_compiler::RequestLog::kRequestBytes + 1, 'z'));
    }
    const char torn[] = {0x20, 0, 0, 0, 1, 2, 3, 4, 't', 'o', 'r', 'n'};
    std::ofstream(path, std::ios::app | std::ios::binary).write(torn, sizeof torn);
    switch_compiler::RequestLog log;
    ASSERT_TRUE(log.open(path));
    std::vector<std::string> seen;
    log.replay([&](const std::string &request, raw::Digest &name) {
        seen.push_back(request);
        name = digest(uint8_t(0x60 + seen.size()));
        return request != "request 1";
    });
    EXPECT_EQ(seen, (std::vector<std::string>{"request 0", "request 1", "request 2"}));
    log.add(digest(0x90), "request 9");
    switch_compiler::RequestLog again;
    ASSERT_TRUE(again.open(path));
    seen.clear();
    again.replay([&](const std::string &request, raw::Digest &name) {
        seen.push_back(request);
        name = digest(0x99, uint32_t(seen.size()));
        return true;
    });
    EXPECT_EQ(seen, (std::vector<std::string>{"request 0", "request 2", "request 9"}));
}

TEST_F(SharedStore, AFullLogDropsItsOldestRequests)
{
    const std::string path = (root / "requests").string();
    const std::string big(1000000, 'r');
    constexpr uint32_t kRequests = 40;
    {
        switch_compiler::RequestLog log;
        ASSERT_TRUE(log.open(path));
        for (uint32_t i = 0; i < kRequests; ++i)
            log.add(digest(0x40, i), std::to_string(i) + big);
    }
    switch_compiler::RequestLog log;
    ASSERT_TRUE(log.open(path));
    std::vector<std::string> seen;
    log.replay([&](const std::string &request, raw::Digest &name) {
        seen.push_back(request.substr(0, request.size() - big.size()));
        name = digest(0x41, uint32_t(seen.size()));
        return true;
    });
    ASSERT_FALSE(seen.empty());
    EXPECT_LT(seen.size(), kRequests);
    EXPECT_EQ(seen.back(), std::to_string(kRequests - 1));
    EXPECT_NE(seen.front(), "0");
}

}  // namespace
