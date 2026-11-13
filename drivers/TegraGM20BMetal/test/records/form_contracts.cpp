// SPDX-License-Identifier: MIT
// Two contracts of a pack's form records, which the runtime emits as they are:
//  - a fetch-free form blends each colour it draws on FetchBlend with its own registers (SET_BLEND(j) on), and names the
//    iterated blend program of a colour it draws on FetchIteratedBlend (raw_pack.h PipelineFetchFreeForm);
//  - a storage record with depth or stencil tests and no shader tests has early tests on (raw_storage.h
//    usableStorageMetadata refuses it otherwise).
//   form_contracts <pack.bin>
#include <cstdio>
#include <map>
#include <string>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_pack.h"

namespace {

using namespace raw;

constexpr uint32_t kBlend = 0x1360, kEarlyTests = 0x0210;

std::map<uint32_t, uint32_t> registers(const Pack &pack, const PackPipeline &pp)
{
    std::map<uint32_t, uint32_t> out;
    const uint32_t *base = pack.section<uint32_t>(PackBaseline);
    for (uint64_t i = 0; i < pack.bytes(PackBaseline) / 8; i++)
        out[base[2 * i]] = base[2 * i + 1];
    const uint32_t *blob = pack.section<uint32_t>(PackBlob) + pp.blob;
    for (uint32_t i = 0; i + 1 < pp.blobDwords; i += 2)
        out[blob[i]] = blob[i + 1];
    return out;
}

uint32_t value(const std::map<uint32_t, uint32_t> &regs, uint32_t method)
{
    const auto it = regs.find(method);
    return it == regs.end() ? 0 : it->second;
}

// What a fetch-free record breaks, "" if nothing
std::string fetchFree(const Pack &pack, const PackPipeline &pp)
{
    const auto regs = registers(pack, pp);
    std::string broken;
    bool blends = false;
    for (uint32_t j = 0; j < 8; j++) {
        const uint8_t rung = pp.fetchRung[j] & kFetchRungMask;
        blends |= rung == FetchBlend || rung == FetchIteratedBlend;
        if (rung == FetchBlend && value(regs, kBlend + 4 * j) != 1)
            broken += " colour " + std::to_string(j) + " on FetchBlend with SET_BLEND off;";
        if (rung == FetchIteratedBlend && !pack.iterated(pp))
            broken += " colour " + std::to_string(j) + " on FetchIteratedBlend with no iterated blend;";
    }
    return blends ? broken : " no colour on FetchBlend or FetchIteratedBlend;";
}

}  // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: form_contracts <pack.bin>\n");
        return 2;
    }
    Pack pack;
    std::string err;
    if (!pack.load(argv[1], 0, err)) {
        printf("%s: %s\n", argv[1], err.c_str());
        return 1;
    }
    const PackPipeline *p = pack.section<PackPipeline>(PackPipelines);
    uint32_t fetchFrees = 0, testedStorage = 0, broken = 0;
    for (uint32_t i = 0; i < pack.header()->pipelines; i++) {
        std::string why;
        if (recordForm(p[i]) == PackForm::FetchFree) {
            fetchFrees++;
            why = fetchFree(pack, p[i]);
        }
        if (p[i].storageMask && p[i].storageTests >= 2 && !p[i].shaderTests.flags) {
            testedStorage++;
            if (!value(registers(pack, p[i]), kEarlyTests))
                why += " storage record with tests and early tests off;";
        }
        if (!why.empty() && broken++ < 20)
            printf("%016llx%s %s\n", (unsigned long long)p[i].keyHash, why.c_str(),
                   std::string(pack.string(p[i].keyString)).substr(0, 100).c_str());
    }
    printf("%u fetch-free records, %u storage records with tests: %u break their form's contract\n", fetchFrees,
           testedStorage, broken);
    return broken || !fetchFrees || !testedStorage ? 1 : 0;
}
