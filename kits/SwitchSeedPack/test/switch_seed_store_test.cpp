// SPDX-License-Identifier: MIT
#include "../SwitchSeedStore.h"

#include <gtest/gtest.h>

#include <signal.h>
#include <sys/wait.h>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <vector>

namespace {

namespace pack = switch_seed_pack;

constexpr uint32_t kSlots = 64;
constexpr uint64_t kData = 64 * 1024;

std::vector<uint8_t> nameOf(uint32_t n)
{
    std::vector<uint8_t> name(pack::kStoreNameBytes);
    std::mt19937 random(n * 2654435761u + 1);
    for (uint8_t &b : name)
        b = uint8_t(random());
    return name;
}

std::string bytesOf(uint32_t n)
{
    return "entry " + std::to_string(n) + "|" + std::string(n % 300, char('a' + n % 26));
}

class SeedStore : public testing::Test {
protected:
    std::filesystem::path root;
    std::string path;
    uint8_t identity[pack::kIdentityBytes] = {};

    void SetUp() override
    {
        std::string pattern = std::string(getenv("TEST_TMPDIR") ?: "/tmp") + "/seed-store-XXXXXX";
        ASSERT_NE(mkdtemp(pattern.data()), nullptr);
        root = pattern;
        path = (root / "store").string();
        identity[0] = 0x53;
    }

    void TearDown() override
    {
        std::filesystem::remove_all(root);
    }

    void open(pack::StoreWriter &writer, const uint8_t *id = nullptr)
    {
        std::string error;
        ASSERT_TRUE(writer.open(path, id ? id : identity, kSlots, kData, error)) << error;
    }

    static pack::StoreWriter::Put put(pack::StoreWriter &writer, uint32_t n)
    {
        const std::string bytes = bytesOf(n);
        return writer.put(nameOf(n).data(), reinterpret_cast<const uint8_t *>(bytes.data()), uint32_t(bytes.size()));
    }

    static bool holds(const pack::StoreReader &reader, uint32_t n)
    {
        pack::Bytes found;
        return reader.find(nameOf(n).data(), found) &&
               std::string(reinterpret_cast<const char *>(found.data), found.size) == bytesOf(n);
    }

    // The entry under `name` is the one bytesOf wrote under it: its number read back from its bytes names it
    static bool whole(const pack::StoreReader &reader, const uint8_t *name)
    {
        pack::Bytes found;
        if (!reader.find(name, found))
            return false;
        const std::string text(reinterpret_cast<const char *>(found.data), found.size);
        const uint32_t n = uint32_t(strtoul(text.c_str() + 6, nullptr, 10));
        return text == bytesOf(n) && !memcmp(nameOf(n).data(), name, pack::kStoreNameBytes);
    }

    void openIn(pack::StoreWriter &writer, const char *boot, uint64_t data = kData)
    {
        char named[pack::kStoreBootBytes] = {};
        strncpy(named, boot, sizeof named - 1);
        std::string error;
        ASSERT_TRUE(writer.open(path, identity, kSlots, data, error, named)) << error;
    }

    // The file mapped read only, as a client's mapping of the kernel's view of it
    struct ReadOnly {
        pack::Bytes mapped;
        pack::StoreReader reader;

        ReadOnly(const std::string &path, const uint8_t *identity)
        {
            bool absent = false;
            EXPECT_TRUE(pack::mapFile(path, mapped, absent));
            reader.attach(mapped.data, mapped.size, identity);
        }

        ~ReadOnly()
        {
            pack::unmap(mapped);
        }
    };
};

TEST_F(SeedStore, AnEntryPutIsFoundAndAnotherOfItsNameIsNotAdded)
{
    pack::StoreWriter writer;
    open(writer);
    EXPECT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
    EXPECT_EQ(put(writer, 1), pack::StoreWriter::Put::Present);
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_FALSE(holds(writer.reader(), 2));
    EXPECT_EQ(writer.used(), 1u);
}

TEST_F(SeedStore, AReaderMappedBeforeAnAppendSeesItWithoutRemapping)
{
    pack::StoreWriter writer;
    open(writer);
    ReadOnly client(path, identity);
    ASSERT_TRUE(client.reader.attached());
    EXPECT_FALSE(holds(client.reader, 7));
    for (uint32_t n = 0; n < 20; ++n)
        ASSERT_EQ(put(writer, n), pack::StoreWriter::Put::Added);
    for (uint32_t n = 0; n < 20; ++n)
        EXPECT_TRUE(holds(client.reader, n)) << n;
}

TEST_F(SeedStore, ASlotNamedButNotPublishedIsEmptyAndTakenAgain)
{
    {
        pack::StoreWriter writer;
        open(writer);
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        // a writer killed after the slot's name but before its offset, its bytes past the published end
        const std::vector<uint8_t> name = nameOf(2);
        const uint32_t slot = pack::firstSlot(name.data(), kSlots);
        uint8_t *entry = writer.base() + pack::kStoreHeaderBytes + uint64_t(slot) * pack::kStoreSlotBytes;
        if (pack::loadAcquire(entry))
            GTEST_SKIP() << "names 1 and 2 share a first slot";
        memcpy(entry + 16, name.data(), name.size());
        memset(writer.base() + writer.layout().dataOffset + writer.dataUsed() + 64, 0xee, 100);
    }
    ReadOnly client(path, identity);
    EXPECT_TRUE(holds(client.reader, 1));
    EXPECT_FALSE(holds(client.reader, 2));
    pack::StoreWriter again;
    open(again);
    EXPECT_EQ(again.used(), 1u);
    EXPECT_EQ(put(again, 2), pack::StoreWriter::Put::Added);
    EXPECT_TRUE(holds(client.reader, 2));
    EXPECT_TRUE(holds(again.reader(), 2));
}

TEST_F(SeedStore, AWriterKilledMidAppendLeavesEveryPublishedEntryWhole)
{
    {
        pack::StoreWriter writer;
        open(writer);
    }
    for (uint32_t round = 0; round < 8; ++round) {
        const pid_t child = fork();
        ASSERT_GE(child, 0);
        if (!child) {
            pack::StoreWriter writer;
            std::string error;
            if (!writer.open(path, identity, kSlots, kData, error))
                _exit(2);
            for (uint32_t n = round * 1000;; ++n)
                if (put(writer, n) == pack::StoreWriter::Put::Full) {
                    writer.retire(nameOf(n - 1).data());
                    n = round * 1000 + 500;
                }
        }
        usleep(200 + round * 300);
        kill(child, SIGKILL);
        int status = 0;
        waitpid(child, &status, 0);
        pack::StoreWriter writer;
        open(writer);
        const pack::StoreLayout &layout = writer.layout();
        for (uint32_t slot = 0; slot < layout.slots; ++slot) {
            const uint8_t *entry = writer.base() + pack::kStoreHeaderBytes + uint64_t(slot) * pack::kStoreSlotBytes;
            const uint64_t offset = pack::loadAcquire(entry);
            if (offset && offset != pack::kRetired)
                EXPECT_TRUE(whole(writer.reader(), entry + 16)) << "round " << round << " slot " << slot;
        }
        if (writer.used() + writer.retired() + 1 < kSlots * 3 / 4)
            EXPECT_EQ(put(writer, 900000 + round), pack::StoreWriter::Put::Added);
    }
}

TEST_F(SeedStore, AStoreOfAnotherIdentityIsNotReadAndIsMadeAgain)
{
    {
        pack::StoreWriter writer;
        open(writer);
        ASSERT_EQ(put(writer, 3), pack::StoreWriter::Put::Added);
    }
    uint8_t other[pack::kIdentityBytes] = {0x54};
    ReadOnly stale(path, other);
    EXPECT_FALSE(stale.reader.attached());
    pack::StoreWriter writer;
    open(writer, other);
    EXPECT_EQ(writer.used(), 0u);
    EXPECT_FALSE(holds(writer.reader(), 3));
}

TEST_F(SeedStore, AStoreMadeAgainLeavesAReaderOfTheOldOneWhole)
{
    {
        pack::StoreWriter writer;
        open(writer);
        for (uint32_t n = 0; n < 8; ++n)
            ASSERT_EQ(put(writer, n), pack::StoreWriter::Put::Added);
    }
    ReadOnly old(path, identity);
    ASSERT_TRUE(old.reader.attached());
    uint8_t other[pack::kIdentityBytes] = {0x54};
    pack::StoreWriter writer;
    open(writer, other);
    ASSERT_EQ(put(writer, 100), pack::StoreWriter::Put::Added);
    for (uint32_t n = 0; n < 8; ++n)
        EXPECT_TRUE(holds(old.reader, n)) << n;
    EXPECT_FALSE(holds(old.reader, 100));
    ReadOnly made(path, other);
    EXPECT_TRUE(holds(made.reader, 100));
    EXPECT_FALSE(std::filesystem::exists(path + ".new"));
}

TEST_F(SeedStore, ARetiredEntryIsSteppedOverAndMayBePublishedAgain)
{
    pack::StoreWriter writer;
    open(writer);
    ASSERT_EQ(put(writer, 4), pack::StoreWriter::Put::Added);
    ASSERT_EQ(put(writer, 5), pack::StoreWriter::Put::Added);
    EXPECT_TRUE(writer.retire(nameOf(4).data()));
    EXPECT_FALSE(holds(writer.reader(), 4));
    EXPECT_TRUE(holds(writer.reader(), 5));
    EXPECT_EQ(put(writer, 4), pack::StoreWriter::Put::Added);
    EXPECT_TRUE(holds(writer.reader(), 4));
}

TEST_F(SeedStore, AFullStoreRefusesAndCompactionKeepsItsLiveEntries)
{
    {
        pack::StoreWriter writer;
        open(writer);
        uint32_t n = 0;
        while (put(writer, n) == pack::StoreWriter::Put::Added)
            ++n;
        EXPECT_EQ(put(writer, n), pack::StoreWriter::Put::Full);
        EXPECT_EQ(writer.used(), kSlots * 3 / 4);
        writer.retire(nameOf(0).data());
    }
    std::string error;
    ASSERT_TRUE(pack::compactStore(path, identity, kSlots, kData, error)) << error;
    pack::StoreWriter writer;
    open(writer);
    EXPECT_EQ(writer.used(), kSlots * 3 / 4 - 1);
    EXPECT_EQ(writer.retired(), 0u);
    EXPECT_FALSE(holds(writer.reader(), 0));
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_TRUE(holds(writer.reader(), 2));
}

// A panic after entry 3 was published and before it was persisted: its slot may be on the card without its bytes
TEST_F(SeedStore, AnEntryPastTheDurableEndIsRetiredByAWriterOfALaterBoot)
{
    uint64_t durable = 0;
    {
        pack::StoreWriter writer;
        openIn(writer, "boot A");
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
        const uint64_t written = writer.layout().dataOffset + writer.dataUsed();
        ASSERT_TRUE(writer.persist());
        durable = writer.durableEnd();
        EXPECT_EQ(durable, written);
        ASSERT_EQ(put(writer, 3), pack::StoreWriter::Put::Added);
    }
    pack::StoreWriter writer;
    openIn(writer, "boot B");
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_TRUE(holds(writer.reader(), 2));
    EXPECT_FALSE(holds(writer.reader(), 3));
    EXPECT_EQ(writer.used(), 2u);
    EXPECT_EQ(writer.retired(), 1u);
    EXPECT_EQ(writer.durableEnd(), durable);
    ASSERT_EQ(put(writer, 3), pack::StoreWriter::Put::Added);
    pack::Bytes found;
    ASSERT_TRUE(writer.reader().find(nameOf(3).data(), found));
    EXPECT_GE(uint64_t(found.data - writer.base()), durable) << "a new entry lands past what a later boot trusts";
    EXPECT_EQ(uint64_t(found.data - writer.base()) % pack::kStorePageBytes, 0u) << "and off the mark's page";
}

uint64_t offsetOf(const pack::StoreWriter &writer, uint32_t n)
{
    pack::Bytes found;
    return writer.reader().find(nameOf(n).data(), found) ? uint64_t(found.data - writer.base()) : 0;
}

// A torn write of the page an entry shares with trusted bytes would corrupt them with nothing to see it
TEST_F(SeedStore, AnEntryPutAfterAPersistStartsOnAPageOfItsOwn)
{
    pack::StoreWriter writer;
    openIn(writer, "boot A");
    ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
    ASSERT_TRUE(writer.persist());
    const uint64_t durable = writer.durableEnd();
    ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
    EXPECT_EQ(offsetOf(writer, 2) % pack::kStorePageBytes, 0u);
    EXPECT_GE(offsetOf(writer, 2), durable);
    ASSERT_EQ(put(writer, 3), pack::StoreWriter::Put::Added);
    EXPECT_EQ(offsetOf(writer, 3), offsetOf(writer, 2) + bytesOf(2).size() + (8 - bytesOf(2).size() % 8) % 8)
        << "entries between persists are packed";
}

// The compiler restarted in the boot that wrote the store, its newest entries retired (a refusal made again): the
// bytes below the mark are a later boot's to trust, so nothing is written there again
TEST_F(SeedStore, ASameBootWriterWhoseNewestEntriesWereRetiredAppendsPastTheMark)
{
    uint64_t durable = 0;
    {
        pack::StoreWriter writer;
        openIn(writer, "boot A");
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
        durable = writer.durableEnd();
        ASSERT_TRUE(writer.retire(nameOf(2).data()));
    }
    pack::StoreWriter writer;
    openIn(writer, "boot A");
    ASSERT_EQ(put(writer, 3), pack::StoreWriter::Put::Added);
    EXPECT_GE(offsetOf(writer, 3), durable);
    EXPECT_EQ(offsetOf(writer, 3) % pack::kStorePageBytes, 0u);
    EXPECT_TRUE(holds(writer.reader(), 1));
}

// The newer mark's write torn so its generation is the old one's: the older mark is read, which trusts less
TEST_F(SeedStore, ATornNewerMarkFallsBackToTheOlderMark)
{
    uint64_t older = 0;
    {
        pack::StoreWriter writer;
        openIn(writer, "boot A");
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
        older = writer.durableEnd();
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
        ASSERT_GT(writer.durableEnd(), older);
        uint8_t *newer = writer.base() + pack::kStoreMarksAt;  // generation 2's slot
        memset(newer + 8, 0, 8);
    }
    pack::StoreWriter writer;
    openIn(writer, "boot B");
    EXPECT_EQ(writer.durableEnd(), older);
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_FALSE(holds(writer.reader(), 2));
}

// The compiler started again in the boot that wrote the store: the page cache holds every published entry whole
TEST_F(SeedStore, AWriterOfTheSameBootKeepsEntriesPastTheDurableEnd)
{
    {
        pack::StoreWriter writer;
        openIn(writer, "boot A");
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
    }
    pack::StoreWriter writer;
    openIn(writer, "boot A");
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_TRUE(holds(writer.reader(), 2));
    EXPECT_EQ(writer.retired(), 0u);
}

TEST_F(SeedStore, AStoreThatSaysNothingOfItsBootTrustsOnlyItsDurableEnd)
{
    {
        pack::StoreWriter writer;
        openIn(writer, "");
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
    }
    pack::StoreWriter writer;
    openIn(writer, "");
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_FALSE(holds(writer.reader(), 2));
}

TEST_F(SeedStore, APersistWithNothingNewKeepsTheMark)
{
    pack::StoreWriter writer;
    openIn(writer, "boot A");
    ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
    ASSERT_TRUE(writer.persist());
    const uint64_t durable = writer.durableEnd();
    EXPECT_TRUE(writer.persist());
    EXPECT_EQ(writer.durableEnd(), durable);
}

TEST_F(SeedStore, AStoreWrittenAgainIsDurableWhole)
{
    {
        pack::StoreWriter writer;
        openIn(writer, "boot A");
        for (uint32_t n = 0; n < 6; ++n)
            ASSERT_EQ(put(writer, n), pack::StoreWriter::Put::Added);
        ASSERT_TRUE(writer.persist());
    }
    std::string error;
    ASSERT_TRUE(pack::compactStore(path, identity, kSlots, kData, error)) << error;
    pack::StoreWriter writer;
    openIn(writer, "boot B");
    for (uint32_t n = 0; n < 6; ++n)
        EXPECT_TRUE(holds(writer.reader(), n)) << n;
    EXPECT_EQ(writer.retired(), 0u);
}

// A program slot kept for a GPU client to run in place: a whole-page entry on pages no other entry shares, before and
// after compaction
TEST_F(SeedStore, AWholePageEntryHasItsPagesToItself)
{
    const auto pageOf = [](uint64_t offset) {
        return offset / pack::kStorePageBytes;
    };
    const std::string page(pack::kStorePageBytes, 'p');
    std::vector<uint8_t> slotName = nameOf(1000);
    {
        pack::StoreWriter writer;
        std::string error;
        ASSERT_TRUE(writer.open(path, identity, kSlots, 4 * kData, error)) << error;
        ASSERT_EQ(put(writer, 1), pack::StoreWriter::Put::Added);
        ASSERT_EQ(writer.put(slotName.data(), reinterpret_cast<const uint8_t *>(page.data()), uint32_t(page.size())),
                  pack::StoreWriter::Put::Added);
        ASSERT_EQ(put(writer, 2), pack::StoreWriter::Put::Added);
        pack::Bytes slot, before, after;
        ASSERT_TRUE(writer.reader().find(slotName.data(), slot));
        ASSERT_TRUE(writer.reader().find(nameOf(1).data(), before));
        ASSERT_TRUE(writer.reader().find(nameOf(2).data(), after));
        const uint64_t at = uint64_t(slot.data - writer.base());
        EXPECT_EQ(at % pack::kStorePageBytes, 0u);
        EXPECT_LT(pageOf(uint64_t(before.data - writer.base()) + before.size - 1), pageOf(at));
        EXPECT_GE(uint64_t(after.data - writer.base()), at + page.size()) << "the next entry starts past its pages";
    }
    std::string error;
    ASSERT_TRUE(pack::compactStore(path, identity, kSlots, 4 * kData, error)) << error;
    pack::StoreWriter writer;
    ASSERT_TRUE(writer.open(path, identity, kSlots, 4 * kData, error)) << error;
    pack::Bytes slot;
    ASSERT_TRUE(writer.reader().find(slotName.data(), slot));
    EXPECT_EQ(uint64_t(slot.data - writer.base()) % pack::kStorePageBytes, 0u);
    EXPECT_TRUE(holds(writer.reader(), 1));
    EXPECT_TRUE(holds(writer.reader(), 2));
}

TEST_F(SeedStore, AReplacementIsPublishedBeforeTheEntryItReplacesIsRetired)
{
    pack::StoreWriter writer;
    open(writer);
    ReadOnly client(path, identity);
    ASSERT_EQ(put(writer, 6), pack::StoreWriter::Put::Added);
    const std::string longer = bytesOf(6) + " with its split";
    ASSERT_EQ(
        writer.put(nameOf(6).data(), reinterpret_cast<const uint8_t *>(longer.data()), uint32_t(longer.size()), true),
        pack::StoreWriter::Put::Added);
    pack::Bytes found;
    ASSERT_TRUE(client.reader.find(nameOf(6).data(), found));
    EXPECT_EQ(std::string(reinterpret_cast<const char *>(found.data), found.size), longer);
    EXPECT_EQ(writer.used(), 1u);
    EXPECT_EQ(writer.retired(), 1u);
}

TEST_F(SeedStore, OneWriterAtATime)
{
    pack::StoreWriter first, second;
    open(first);
    std::string error;
    EXPECT_FALSE(second.open(path, identity, kSlots, kData, error));
}

}  // namespace
