// SPDX-License-Identifier: MIT
// emitcheck: runtime/emit.h's registers against every record of a pack but its form records, as a direct compile
// would make them for a state the pack lacks. A program's register count is its PackProgram's; its early-fragment-tests
// (what NAK's shader info will give a direct compile) are read off the records that name it, and must agree among them.
// Every register the hardware reads must be the pack's; one it doesn't read may hold what another state left there.
// usage: emitcheck <pack.bin>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "../emit.h"
#include "../pack.h"

namespace {

constexpr uint32_t kEarlyZ = 0x0210;

// A fetch-free form's record is its form's (pack/fetch_free_make.h: its pipeline blend, colour targets and fetch
// registers), as a storage record's early tests, sample control, interlock and storage writes are (storage.h
// usableStorageMetadata): neither is what the state's key emits. The record goldens hold them (test/records).
bool formRecord(const gm20b::PackPipeline &pp)
{
    return gm20b::recordForm(pp) == gm20b::PackForm::FetchFree || pp.storageMask;
}

std::map<uint32_t, uint32_t> registers(const gm20b::Pack &pack, const gm20b::PackPipeline &pp)
{
    std::map<uint32_t, uint32_t> regs;
    const uint32_t *blob = pack.section<uint32_t>(gm20b::PackBlob) + pp.blob;
    for (uint32_t k = 0; k + 1 < pp.blobDwords; k += 2)
        regs[blob[k]] = blob[k + 1];
    return regs;
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: emitcheck <pack.bin>\n");
        return 2;
    }
    gm20b::Pack pack;
    std::string err;
    if (!pack.load(argv[1], 0, err)) {
        fprintf(stderr, "emitcheck: %s\n", err.c_str());
        return 1;
    }
    const gm20b::PackPipeline *pipes = pack.section<gm20b::PackPipeline>(gm20b::PackPipelines);
    const gm20b::PackProgram *programs = pack.section<gm20b::PackProgram>(gm20b::PackPrograms);
    const uint8_t *heap = pack.section<uint8_t>(gm20b::PackHeap);
    const uint32_t count = pack.header()->pipelines;

    std::map<uint32_t, gm20b::EmitProgram> info;
    uint32_t disagree = 0;
    auto learn = [&](uint32_t prog, uint32_t gprs, bool earlyZ, bool fragment, uint32_t interlock, bool sampleShading) {
        const auto [it, first] = info.try_emplace(prog);
        if (first) {
            it->second.header = heap + programs[prog].program;
            it->second.gprs = gprs;
            it->second.earlyFragmentTests = earlyZ;
            it->second.interlockControl = interlock;
            it->second.usesSampleShading = sampleShading;
        } else {
            disagree +=
                it->second.gprs != gprs ||
                (fragment && (it->second.earlyFragmentTests != earlyZ || it->second.interlockControl != interlock));
        }
    };
    uint32_t forms = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (formRecord(pipes[i])) {
            forms++;
            continue;
        }
        const std::map<uint32_t, uint32_t> regs = registers(pack, pipes[i]);
        learn(pipes[i].vs, programs[pipes[i].vs].gprs, false, false, 0, false);
        if (gm20b::pixelStage(pipes[i]))
            learn(pipes[i].fs, programs[pipes[i].fs].gprs, regs.count(kEarlyZ) && regs.at(kEarlyZ), true,
                  regs.count(0x1224) ? regs.at(0x1224) : 0, regs.count(0x0754) && (regs.at(0x0754) & 15) > 1);
    }

    uint64_t compared = 0, read = 0, unread = 0, unparsed = 0;
    std::map<uint32_t, uint64_t> readBad, unreadBad;
    std::set<uint32_t> covered, left;
    for (uint32_t i = 0; i < count; i++) {
        if (formRecord(pipes[i]))
            continue;
        const std::map<uint32_t, uint32_t> regs = registers(pack, pipes[i]);
        std::vector<gm20b::EmitRegister> made;
        std::string why;
        const gm20b::EmitProgram none, &fragment = gm20b::pixelStage(pipes[i]) ? info[pipes[i].fs] : none;
        if (!gm20b::emitState(pack.string(pipes[i].keyString), info[pipes[i].vs], fragment, made, why)) {
            if (!unparsed++)
                printf("emitcheck: %s\n", why.c_str());
            continue;
        }
        std::set<uint32_t> emitted;
        for (const gm20b::EmitRegister &r : made) {
            emitted.insert(r.method);
            const auto it = regs.find(r.method);
            if (it == regs.end())
                continue;
            covered.insert(r.method);
            compared++;
            if (it->second == r.value)
                continue;
            if (r.read) {
                read++;
                if (!readBad[r.method]++)
                    printf("DIFFERS %04x pack %x made %x: %s\n", r.method, it->second, r.value,
                           pack.string(pipes[i].keyString));
            } else {
                unread++;
                unreadBad[r.method]++;
            }
        }
        for (const auto &[m, v] : regs)
            if (!emitted.count(m))
                left.insert(m);
    }
    printf(
        "emitcheck %s: %u records (%u fetch-free or storage forms left out), %zu programs (%u disagree on their info), "
        "%zu registers made, %llu values compared: %llu read ones differ, %llu unread ones differ, %llu keys unread\n",
        argv[1], count, forms, info.size(), disagree, covered.size(), (unsigned long long)compared,
        (unsigned long long)read, (unsigned long long)unread, (unsigned long long)unparsed);
    printf("  not made yet:");
    for (uint32_t m : left)
        printf(" %04x", m);
    printf("\n  unread differences:");
    for (const auto &[m, n] : unreadBad)
        printf(" %04x x%llu", m, (unsigned long long)n);
    printf("\n");
    return read || disagree || unparsed ? 1 : 0;
}
