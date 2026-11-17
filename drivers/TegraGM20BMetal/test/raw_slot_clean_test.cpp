// SPDX-License-Identifier: MIT
// Which program slots run in place are cleaned from the CPU caches when placed (Device::placeInPlace), on a client device
// over the kernel model. The pack's pages are cleaned once by the kernel at their import and no process writes them, so a
// pack slot placed in a grain bound already is not cleaned again (perf review N9: ~0.1 ms a state's first use). A slot of
// the shader compiler's store is cleaned at each placement: the compiler writes the store after a grain of it may have
// been imported, including the part of a slot in a grain an earlier slot bound.
#include "raw_kernel_model.h"
#include "../raw/runtime/raw_gpu.h"
#include "../raw/runtime/raw_pack.h"
#include "../raw/runtime/raw_program_pages.h"
#include "../raw/runtime/raw_record.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kStateA = 0xa0, kStateB = 0xb0;
constexpr uint32_t kHeap = 0x1000, kLarge = 0x1000, kSmall = 0x100;

// Programs 0 (a model page: runs in place, importing its grain), 1 and 2 (small, in that grain); state A draws 0 and 2,
// state B 1 and 2
std::string packOfThreePrograms()
{
    constexpr uint32_t kPrograms = 0x400, kPipelines = 0x500, kStrings = 0x900, kBlob = 0xa00;
    const uint32_t heapBytes = kLarge + 2 * kSmall + uint32_t(raw::kProgramPrefetchBytes);
    std::vector<uint8_t> file(kHeap + heapBytes, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.programs = 3;
    h.pipelines = 2;
    raw::PackProgram programs[3] = {};
    const uint32_t offsets[3] = {0, kLarge, kLarge + kSmall}, sizes[3] = {kLarge, kSmall, kSmall};
    for (uint32_t i = 0; i < 3; i++) {
        programs[i].heapOffset = offsets[i];
        programs[i].program = offsets[i] + 0x30;
        programs[i].bytes = sizes[i];
        programs[i].gprs = 8;
        programs[i].cbufs = 1;
        programs[i].cbufType[0] = raw::CbufRoot;
        for (uint32_t b = 0x80; b < sizes[i]; b++)
            file[kHeap + offsets[i] + b] = uint8_t(b * 7 + i);
    }
    raw::PackPipeline pp[2] = {};
    const uint64_t keys[2] = {kStateA, kStateB};
    const uint16_t vertex[2] = {0, 1};
    for (int i = 0; i < 2; i++) {
        pp[i].keyHash = keys[i];
        pp[i].keyString = 1;
        pp[i].vs = vertex[i];
        pp[i].fs = 2;
        pp[i].blobDwords = 2;
        for (raw::PackUniformRange &u : pp[i].uniform)
            u.entry = raw::kNoUniform;
    }
    const char strings[] = "\0samples=1 c0=70 ds=0/0";
    const uint32_t blob[] = {0x1a00, 0x1111};
    const auto put = [&](raw::PackSectionId id, uint32_t at, const void *bytes, size_t n) {
        h.section[id] = {at, n};
        memcpy(file.data() + at, bytes, n);
    };
    put(raw::PackPrograms, kPrograms, programs, sizeof programs);
    put(raw::PackPipelines, kPipelines, pp, sizeof pp);
    put(raw::PackStrings, kStrings, strings, sizeof strings);
    put(raw::PackBlob, kBlob, blob, sizeof blob);
    h.section[raw::PackHeap] = {kHeap, heapBytes};
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/three-program-pack.bin";
    std::ofstream(path, std::ios::binary)
        .write(reinterpret_cast<const char *>(file.data()), std::streamsize(file.size()));
    return path;
}

raw::CapturedProgram program(const char *id, size_t bytes, uint8_t fill)
{
    raw::CapturedProgram p;
    p.id = id;
    p.image.assign(bytes, 0);
    for (size_t i = 80; i < bytes; ++i)
        p.image[i] = uint8_t(fill + i);
    return p;
}

class SlotCleans : public testing::Test {
protected:
    raw::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = 1ull << 40;
        std::string err;
        ASSERT_TRUE(dev.open(packOfThreePrograms().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    void TearDown() override
    {
        dev.runSlotsFrom(nullptr);
    }

    uint64_t cleans() const
    {
        return dev.stats.slotCleans.load();
    }
};

// State A's large vertex program imports the grain the small programs lie in; its fragment program and state B's vertex
// program then run in place from that bound grain, and neither is cleaned
TEST_F(SlotCleans, APackSlotInAGrainBoundAlreadyIsNotCleanedAgain)
{
    ASSERT_NE(dev.pipeline(kStateA), nullptr);
    ASSERT_NE(dev.pipeline(kStateB), nullptr);
    EXPECT_EQ(dev.stats.packSlotsInBoundGrains.load(), 2u) << "both small programs ran in place from the bound grain";
    EXPECT_EQ(cleans(), 0u);
}

// Two store slots, the second starting in the grain the first bound and ending in it: each is cleaned when placed
TEST_F(SlotCleans, EveryStoreSlotIsCleanedWhereverItsGrainsWereBound)
{
    raw::RuntimeRecord first, second;
    first.keyString = "first";
    first.pipeline.keyHash = 0xf1;
    first.programs[0] = program("vs1", 0x2000, 0x11);
    first.programs[1] = program("fs1", 0x100, 0x22);
    second.keyString = "second";
    second.pipeline.keyHash = 0xf2;
    second.programs[0] = program("vs2", 0x2000, 0x33);
    second.programs[1] = program("fs2", 0x100, 0x44);
    raw::PackProgram info;
    const std::vector<uint8_t> a = raw::programSlot(first.programs[0], info),
                               b = raw::programSlot(second.programs[0], info);
    const size_t bytes = 4 * raw::kProgramGrain;
    void *m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    ASSERT_NE(m, MAP_FAILED);
    uint8_t *store = static_cast<uint8_t *>(m);
    const size_t at[2] = {0, raw::programSlotEntryBytes(a.size())};
    ASSERT_LT(at[1] + b.size(), raw::kProgramGrain) << "both in the first grain";
    memcpy(store + at[0], a.data(), a.size());
    memcpy(store + at[1], b.data(), b.size());
    dev.runSlotsFrom([&](const uint8_t *s, uint32_t size, const uint8_t *&mapping, uint64_t &length) {
        mapping = store;
        length = bytes;
        for (size_t offset : at)
            if (!memcmp(store + offset, s, size))
                return static_cast<const uint8_t *>(store + offset);
        return static_cast<const uint8_t *>(nullptr);
    });
    std::string why;
    ASSERT_NE(dev.addRecord(first, false, why), nullptr) << why;
    ASSERT_NE(dev.addRecord(second, false, why), nullptr) << why;
    EXPECT_EQ(dev.stats.storeSlotsPlaced.load(), 2u);
    EXPECT_EQ(cleans(), 2u) << "the second slot's grain was bound by the first, after which the compiler wrote it";
    dev.runSlotsFrom(nullptr);
    munmap(m, bytes);
}

}  // namespace
