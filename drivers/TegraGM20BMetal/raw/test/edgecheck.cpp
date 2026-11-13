// SPDX-License-Identifier: MIT
#include "../runtime/raw_compute.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unistd.h>
extern "C" bool cap_read(uint64_t, void *, uint64_t);

int main()
{
    char directory[] = "/private/tmp/switch-edge-XXXXXX";
    if (!mkdtemp(directory))
        return 2;
    const std::string root(directory), pack = root + "/pack", push = root + "/push", capture = root + "/capture";
    setenv("RAWCAP_LOG", capture.c_str(), 1);
    setenv("PUSHDUMP", push.c_str(), 1);
    raw::PackHeader header{};
    memcpy(header.magic, "GM20BPK1", 8);
    header.version = raw::kPackVersion;
    for (auto &section : header.section)
        section.offset = sizeof header;
    const raw::PackInitMethod init[] = {{0, 0, 0xb197, 0}, {1, 0, 0xb1c0, 0}, {4, 0, 0xb0b5, 0}};
    header.section[raw::PackInit].bytes = sizeof init;
    std::ofstream packed(pack, std::ios::binary);
    packed.write(reinterpret_cast<char *>(&header), sizeof header);
    packed.write(reinterpret_cast<const char *>(init), sizeof init);
    packed.close();
    raw::Device device;
    std::string error;
    if (!device.open(pack.c_str(), 0, error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 2;
    }
    const std::array plans{raw::computeDispatch({19, 11, 7}, {8, 5, 3}, true),
                           raw::computeDispatch({3, 2, 1}, {8, 5, 3}, true)};
    auto *command = device.begin();
    if (!command)
        return 2;
    raw::ComputeEncoder encoder(*command, device);
    encoder.setSerial(true);
    std::array<raw::PackKernel, 9> records{};
    std::array<raw::ComputeState, 9> states{};
    size_t at = 0;
    for (const auto &plan : plans)
        for (uint8_t i = 0; i < plan.count; ++i, ++at) {
            auto &record = records[at];
            const auto &region = plan.regions[i];
            record.regionWord = 8;
            record.qmd[18] = region.local[0] << 16;
            record.qmd[19] = region.local[1] | region.local[2] << 16;
            record.qmd[20] = 1;
            states[at].rec = &record;
            states[at].instructions = raw::BatchWork::kLimit;
            states[at].cbufType[0] = raw::CbufRoot;
            encoder.setState(&states[at]);
            const char *why = nullptr;
            if (!encoder.dispatchRegion(region, plan.grid, plan.nominal, !i, i + 1 == plan.count, &why)) {
                fprintf(stderr, "%s\n", why);
                return 2;
            }
        }
    encoder.end();
    uint32_t sequence;
    if (!command->commit(sequence) || !device.wait(sequence))
        return 2;
    fflush(nullptr);
    std::ifstream trace(push);
    std::string line;
    unsigned launches = 0, barriers = 0, errors = 0;
    uint64_t firstSubmit = 0;
    while (std::getline(trace, line)) {
        std::istringstream fields(line);
        uint64_t submit;
        uint32_t channel, method, value;
        if (!(fields >> submit >> channel >> std::hex >> method >> value) || channel != 1)
            continue;
        if (method == 0x21c && launches)
            ++barriers;
        if (method != 0x2b4)
            continue;
        if (!launches)
            firstSubmit = submit;
        errors += launches < 8 ? submit != firstSubmit : submit == firstSubmit;
        const auto &plan = plans[launches < 8 ? 0 : 1];
        const auto &part = plan.regions[launches < 8 ? launches : 0];
        uint32_t qmd[64]{}, data[512]{}, expected[8]{};
        const bool readQmd = cap_read(uint64_t(value) << 8, qmd, sizeof qmd);
        const uint64_t address = qmd[29] | uint64_t(qmd[30] & 0xff) << 32;
        const bool readRoot = readQmd && cap_read(address, data, sizeof data);
        raw::computeRegionWords(part.base, plan.grid, plan.nominal, expected);
        const bool exact = readRoot && qmd[12] == part.groups[0] &&
                           qmd[13] == (part.groups[1] | part.groups[2] << 16) && qmd[18] == part.local[0] << 16 &&
                           qmd[19] == (part.local[1] | part.local[2] << 16) &&
                           !memcmp(data + 0x700 / 4 + 8, expected, sizeof expected);
        errors += !exact;
        errors += launches < 8 ? barriers != 0 : barriers != 1;
        ++launches;
    }
    errors += launches != 9;
    printf("%s: %u actual QMD/root snapshots, one serial API boundary, no intra-grid barrier or batch cut\n",
           errors ? "FAIL" : "PASS", launches);
    unlink(pack.c_str());
    unlink(push.c_str());
    unlink(capture.c_str());
    rmdir(root.c_str());
    return errors ? 1 : 0;
}
