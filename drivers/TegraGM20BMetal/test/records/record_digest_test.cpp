// SPDX-License-Identifier: MIT
// record_digest.h over packs this test writes: a record digests by its content wherever the pack builder laid it out and
// whichever of the baseline and its blob carries its registers, a byte of its program, argument table or compute state's
// facts moves its digest and names the section, and the sample does not depend on the records' bytes.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/test/records/golden_files.h"

namespace {

using namespace raw;

struct Built {
    std::vector<PackProgram> programs;
    std::vector<uint8_t> heap;
    std::vector<PackPipeline> pipelines;
    std::vector<PackKernel> kernels;
    std::vector<PackComputeFacts> facts;
    std::vector<uint32_t> blob;
    std::vector<uint32_t> baseline;
    std::string strings = std::string(1, '\0');

    uint32_t string(const std::string &s)
    {
        const auto at = uint32_t(strings.size());
        strings += s + std::string(1, '\0');
        return at;
    }

    // An argument table's words after `gap` words of the blob section; returns where they start
    uint32_t spill(const std::vector<uint32_t> &words, uint32_t gap)
    {
        blob.insert(blob.end(), gap, 0xEEEEEEEE);
        const auto at = uint32_t(blob.size());
        blob.insert(blob.end(), words.begin(), words.end());
        return at;
    }
};

// (method, value) pairs: the pack's baseline, and every state's blob
struct Registers {
    std::vector<uint32_t> baseline, blob;
};

// A program of 0x100 bytes of `fill` but its first, `first`
uint32_t addProgram(Built &b, uint8_t fill, uint8_t first)
{
    PackProgram p = {};
    p.heapOffset = uint32_t(b.heap.size());
    p.program = p.heapOffset + 0x30;
    p.bytes = 0x100;
    p.gprs = 8;
    std::vector<uint8_t> slot(p.bytes, fill);
    slot[0] = first;
    b.heap.insert(b.heap.end(), slot.begin(), slot.end());
    b.programs.push_back(p);
    return uint32_t(b.programs.size() - 1);
}

std::string write(const char *name, const Built &b)
{
    PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = kPackVersion;
    h.programs = uint32_t(b.programs.size());
    h.pipelines = uint32_t(b.pipelines.size());
    std::vector<uint8_t> file(sizeof h);
    const auto append = [&](PackSectionId id, const void *p, size_t n) {
        h.section[id] = {file.size(), n};
        file.insert(file.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
    };
    append(PackPrograms, b.programs.data(), b.programs.size() * sizeof(PackProgram));
    append(PackPipelines, b.pipelines.data(), b.pipelines.size() * sizeof(PackPipeline));
    append(PackKernels, b.kernels.data(), b.kernels.size() * sizeof(PackKernel));
    append(PackComputeStates, b.facts.data(), b.facts.size() * sizeof(PackComputeFacts));
    append(PackStrings, b.strings.data(), b.strings.size());
    append(PackHeap, b.heap.data(), b.heap.size());
    append(PackBlob, b.blob.data(), b.blob.size() * 4);
    append(PackBaseline, b.baseline.data(), b.baseline.size() * 4);
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/" + name;
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), file.size());
    return path;
}

// A pack of one state per key, each with two programs of `code` bytes; `gap` bytes of padding before the heap's first slot
// and the programs in reverse order, so two packs of the same records differ only in layout. `spill`: each state's
// fragment argument table, after `spillGap` words of the blob section
std::string writePack(const char *name, const std::vector<uint64_t> &keys, uint32_t gap, uint8_t fsByte = 0x11,
                      const Registers &registers = {}, const std::vector<uint32_t> &spill = {}, uint32_t spillGap = 0)
{
    Built b;
    b.heap.assign(gap, 0xEE);
    b.baseline = registers.baseline;
    for (uint64_t key : keys) {
        PackPipeline pp = {};
        pp.fs = addProgram(b, fsByte, uint8_t(key));
        pp.vs = addProgram(b, 0x22, uint8_t(key));
        pp.keyHash = key;
        pp.keyString = b.string("state " + std::to_string(key));
        pp.blob = uint32_t(b.blob.size());
        pp.blobDwords = uint32_t(registers.blob.size());
        b.blob.insert(b.blob.end(), registers.blob.begin(), registers.blob.end());
        if (!spill.empty()) {
            pp.spillEntry[1] = 1;
            pp.spillWords[1] = uint8_t(spill.size());
            pp.spillAt = b.spill(spill, spillGap);
        }
        b.pipelines.push_back(pp);
    }
    return write(name, b);
}

constexpr char kComputeState[] = "00112233 | c=1 | max=64 tgm=0";

// A pack of one kernel of kComputeState, its argument table after `spillGap` words of the blob section and its state's
// facts answering `maxThreads`
std::string writeKernelPack(const char *name, uint32_t maxThreads, uint32_t spillGap, uint32_t gap = 0)
{
    Built b;
    b.heap.assign(gap, 0xEE);
    b.string("padding " + std::to_string(gap));
    PackKernel k = {};
    k.program = addProgram(b, 0x33, 0x44);
    const std::string state = kComputeState, key = state + " | tg=8,1,1 | tgmem= | tex= | smp=";
    k.keyString = b.string(key);
    k.keyHash = Pack::fnv(key.data(), key.size());
    k.spillEntry = 1;
    k.spillWords = 2;
    k.spillAt = b.spill({0x10, 0x20}, spillGap);
    b.kernels.push_back(k);
    b.facts.push_back({Pack::fnv(state.data(), state.size()), maxThreads, 32, 0, 0});
    return write(name, b);
}

std::vector<records::Record> load(const std::string &path)
{
    Pack pack;
    std::string err;
    EXPECT_TRUE(pack.load(path.c_str(), 0, err)) << err;
    return records::records(pack);
}

const records::Record &state(const std::vector<records::Record> &all, uint64_t key)
{
    for (const records::Record &r : all)
        if (r.key == key && r.kind == records::Kind::Default)
            return r;
    ADD_FAILURE() << "no state " << key;
    return all.front();
}

TEST(RecordDigest, ARecordDigestsTheSameWhereverItsProgramsAreLaidOut)
{
    const auto a = records::rows(load(writePack("a.bin", {5, 9}, 0)));
    const auto b = records::rows(load(writePack("b.bin", {9, 5}, 0x400)));
    ASSERT_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); i++) {
        EXPECT_EQ(a[i].key, b[i].key);
        EXPECT_EQ(a[i].digest, b[i].digest) << records::keyText(a[i].key);
    }
}

TEST(RecordDigest, AProgramByteMovesItsRecordAndNamesTheSection)
{
    const auto before = load(writePack("c.bin", {5}, 0));
    const auto after = load(writePack("d.bin", {5}, 0, 0x12));
    const records::Record &a = state(before, 5), &b = state(after, 5);
    EXPECT_NE(records::digest(a.sections), records::digest(b.sections));
    const auto was = records::sectionHashes(a), now = records::sectionHashes(b);
    EXPECT_NE(was.at("fs"), now.at("fs"));
    EXPECT_EQ(was.at("vs"), now.at("vs"));
    EXPECT_EQ(was.at("registers"), now.at("registers"));
}

TEST(RecordDigest, AStatesRegistersAreTheSameFromTheBaselineOrItsBlob)
{
    const auto registers = [](const char *name, const Registers &registers) {
        return records::sectionHashes(state(load(writePack(name, {5}, 0, 0x11, registers)), 5)).at("registers");
    };
    const auto inBaseline = registers("h.bin", {{0x754, 1}, {}});
    EXPECT_EQ(inBaseline, registers("i.bin", {{}, {0x754, 1}}));
    EXPECT_EQ(inBaseline, registers("j.bin", {{0x754, 2}, {0x754, 1}}));
    EXPECT_NE(inBaseline, registers("k.bin", {{}, {0x754, 2}}));
    EXPECT_EQ(registers("l.bin", {}), registers("m.bin", {{}, {0x754, 0}}));
    const auto digest = [](const char *name, const Registers &registers) {
        return records::digest(state(load(writePack(name, {5}, 0, 0x11, registers)), 5).sections);
    };
    EXPECT_NE(digest("n.bin", {{0x754, 1}, {}}), digest("o.bin", {{}, {0x754, 1}})) << "the full digest keeps the blob";
}

TEST(RecordDigest, TheSampleHoldsNoBlobAndOfThePacksOwnRecordsOnlyInit)
{
    const auto all = load(writePack("p.bin", {5, 9}, 0, 0x11, {{0x754, 1}, {0x1e04, 2}}));
    const std::string text = records::sampleText(all, records::pickSample(all, 100));
    EXPECT_EQ(text.find("\tblob\t"), std::string::npos);
    EXPECT_NE(text.find("\tregisters\t"), std::string::npos);
    const auto packRecord = [&](uint64_t key) {
        return text.find(records::keyText(key) + "\t5\t") != std::string::npos;
    };
    EXPECT_TRUE(packRecord(records::kInitRecord));
    for (uint64_t key :
         {records::kBaselineRecord, records::kSamplersRecord, records::kArchivesRecord, records::kComputeStatesRecord})
        EXPECT_FALSE(packRecord(key)) << key;
}

TEST(RecordDigest, AnArgumentTableDigestsByItsWordsWhereverTheBlobHoldsIt)
{
    const auto spilled = [](const char *name, const std::vector<uint32_t> &words, uint32_t gap) {
        return state(load(writePack(name, {5}, 0, 0x11, {}, words, gap)), 5);
    };
    const records::Record a = spilled("q.bin", {7, 8}, 0), b = spilled("r.bin", {7, 8}, 3);
    const records::Record c = spilled("s.bin", {7, 9}, 0);
    EXPECT_EQ(records::digest(a.sections), records::digest(b.sections));
    const auto was = records::sectionHashes(a), now = records::sectionHashes(c);
    EXPECT_NE(was.at("spills"), now.at("spills"));
    EXPECT_EQ(was.at("record"), now.at("record"));
}

TEST(RecordDigest, AKernelCarriesItsComputeStatesFactsAndArgumentTableWhereverTheyAreLaidOut)
{
    const auto kernel = [](const char *name, uint32_t maxThreads, uint32_t spillGap, uint32_t gap = 0) {
        for (const records::Record &r : load(writeKernelPack(name, maxThreads, spillGap, gap)))
            if (r.kind == records::Kind::Kernel)
                return r;
        ADD_FAILURE() << "no kernel in " << name;
        return records::Record{};
    };
    const records::Record a = kernel("t.bin", 64, 0), b = kernel("u.bin", 64, 5, 0x200);
    EXPECT_EQ(records::digest(a.sections), records::digest(b.sections));
    const auto facts = std::find_if(a.sections.begin(), a.sections.end(), [](const records::Section &s) {
        return s.name == "facts";
    });
    ASSERT_NE(facts, a.sections.end());
    EXPECT_EQ(facts->bytes.size(), sizeof(PackComputeFacts));
    const auto was = records::sectionHashes(a), now = records::sectionHashes(kernel("v.bin", 32, 0));
    EXPECT_NE(was.at("facts"), now.at("facts"));
    EXPECT_EQ(was.at("record"), now.at("record"));
    EXPECT_EQ(was.at("spills"), now.at("spills"));
}

TEST(RecordDigest, TheSampleIsPickedByKeyNotByContent)
{
    std::vector<uint64_t> keys;
    for (uint64_t k = 1; k <= 400; k++)
        keys.push_back(k * 0x9E3779B97F4A7C15ull);
    const auto a = records::pickSample(load(writePack("e.bin", keys, 0)), 40);
    const auto b = records::pickSample(load(writePack("f.bin", keys, 0, 0x13)), 40);
    EXPECT_EQ(a, b);
    EXPECT_GE(a.size(), 12u);
}

TEST(RecordDigest, RowsRoundTripThroughTheirFile)
{
    const auto rows = records::rows(load(writePack("g.bin", {3, 1, 2}, 0)));
    std::vector<records::Row> back;
    ASSERT_TRUE(records::decode(records::encode(rows), back));
    ASSERT_EQ(back.size(), rows.size());
    for (size_t i = 0; i < rows.size(); i++) {
        EXPECT_EQ(back[i].key, rows[i].key);
        EXPECT_EQ(back[i].kind, rows[i].kind);
        EXPECT_EQ(back[i].digest, rows[i].digest);
    }
}

}  // namespace
