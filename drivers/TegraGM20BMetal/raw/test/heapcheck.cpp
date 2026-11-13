// SPDX-License-Identifier: MIT
// The shader heap holds every program of the pack, over the mock GPU: every state of the pack made in one process (its whole
// program image, past any fixed cap), and each state's two programs read back at the GPU VA its SET_PIPELINE_PROGRAM names,
// byte for byte the pack's, each in one bound range, with the 4 KiB the instruction prefetch reads past its end bound as
// soon as it is placed.
// usage: RAWCAP_LOG=/dev/null heapcheck <pack.bin>   (the decoder gives the context's SET_PROGRAM_REGION)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "../runtime/raw_gpu.h"

extern "C" bool cap_read(uint64_t va, void *out, uint64_t bytes);
extern "C" uint32_t cap_reg3d(uint32_t mthd);

namespace {

constexpr uint32_t kProgramRegionA = 0x1608, kProgramRegionB = 0x160c;
constexpr uint32_t kVertexProgram = 0x2044, kPixelProgram = 0x2144;
// raw_gpu.cpp kHeapSlack: how far past a program's end the SMs' instruction prefetch reads
constexpr uint64_t kPrefetch = 0x1000, kPage = 0x1000;
int failures;

void expect(bool ok, const char *what)
{
    std::printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    failures += !ok;
}

// The values a state's push words give `mthd` (subchannel 3D): op 1 incrementing, 3 non-incrementing, 4 immediate, 5 once
bool methodValue(const std::vector<uint32_t> &w, uint32_t mthd, uint32_t &value)
{
    bool found = false;
    for (size_t i = 0; i < w.size();) {
        const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, m = (h & 0x1fff) << 2;
        if (op == 4) {
            if (m == mthd)
                value = count, found = true;
            continue;
        }
        for (uint32_t k = 0; k < count && i < w.size(); k++, i++) {
            const uint32_t at = op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m;
            if (at == mthd)
                value = w[i], found = true;
        }
    }
    return found;
}

// Every page of [va, va + bytes) is bound (the range may cross from one heap part into the next)
bool bound(uint64_t va, uint64_t bytes)
{
    uint8_t page[kPage];
    for (uint64_t at = va, end = va + bytes; at < end;) {
        const uint64_t next = std::min(end, (at + kPage) & ~(kPage - 1));
        if (!cap_read(at, page, next - at))
            return false;
        at = next;
    }
    return true;
}

// The program `prog` of the pack at heap offset `program` (its SET_PIPELINE_PROGRAM value) is the pack's bytes, and the
// instruction prefetch's reach past its end is bound, checked as each state is made (its programs the heap's newest)
uint32_t prefetchCrossings;  // programs whose prefetch reach crosses into the next 256 KiB heap step

bool programAt(const raw::Pack &pack, uint64_t region, uint32_t prog, uint32_t program)
{
    const raw::PackProgram &p = pack.section<raw::PackProgram>(raw::PackPrograms)[prog];
    const uint64_t slot = region + program - (p.program - p.heapOffset);
    const uint64_t end = slot - region + p.bytes;
    prefetchCrossings += end >> 18 != (end + kPrefetch - 1) >> 18;
    std::vector<uint8_t> read(p.bytes);
    return cap_read(slot, read.data(), read.size()) &&
           !memcmp(read.data(), pack.section<uint8_t>(raw::PackHeap) + p.heapOffset, p.bytes) &&
           bound(slot + p.bytes, kPrefetch);
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: RAWCAP_LOG=/dev/null heapcheck <pack.bin>\n");
        return 2;
    }
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[1], 0, err)) {
        std::fprintf(stderr, "open: %s\n", err.c_str());
        return 1;
    }
    std::printf("open: heap bound %llu KiB\n", (unsigned long long)(dev.heapBytes() >> 10));
    uint32_t seq = 0;
    raw::CommandBuffer *cb = dev.begin();
    expect(cb && cb->commit(seq) && dev.wait(seq), "the context runs");
    const uint64_t region = uint64_t(cap_reg3d(kProgramRegionA)) << 32 | cap_reg3d(kProgramRegionB);
    expect(region != 0, "the context sets SET_PROGRAM_REGION");

    const raw::Pack &pack = dev.pack();
    const raw::PackPipeline *records = pack.section<raw::PackPipeline>(raw::PackPipelines);
    const uint32_t count = pack.header()->pipelines;
    uint32_t made = 0, programsRight = 0;
    std::set<uint32_t> named;
    for (uint32_t i = 0; i < count; i++) {
        const raw::PackForm form = raw::recordForm(records[i]);
        const raw::Pipeline *pl = dev.pipeline(records[i].keyHash, form);
        if (records[i].flags & raw::PipelineVariantRecord)
            for (const auto &[name, variant] : dev.variantPipelines(records[i].keyHash, false))
                pl = name == pack.string(records[i].variantName) ? variant : pl;
        if (!pl)
            continue;
        made++;
        named.insert({pl->rec->vs, pl->rec->fs});
        uint32_t vs = 0, fs = 0;
        programsRight += methodValue(pl->methods, kVertexProgram, vs) && methodValue(pl->methods, kPixelProgram, fs) &&
                         programAt(pack, region, pl->rec->vs, vs) && programAt(pack, region, pl->rec->fs, fs);
    }
    std::printf(
        "%u of %u states made, %u with both programs the pack's at their GPU VA (%u with the prefetch past a heap "
        "step); %llu programs placed, heap bound %llu KiB of a %llu KiB image\n",
        made, count, programsRight, prefetchCrossings, (unsigned long long)dev.stats.programsPlaced.load(),
        (unsigned long long)(dev.heapBytes() >> 10), (unsigned long long)(pack.bytes(raw::PackHeap) >> 10));
    expect(made == count && !dev.stats.heapFull.load(), "every state of the pack is made, the heap never full");
    expect(programsRight == count,
           "every state's programs are the pack's bytes at the VA its methods name, the prefetch past each bound");
    expect(dev.stats.programsPlaced.load() == named.size(), "each program a state names is placed once");
    if (failures) {
        std::printf("FAIL: heapcheck (%d)\n", failures);
        return 1;
    }
    std::printf("PASS: heapcheck\n");
    return 0;
}
