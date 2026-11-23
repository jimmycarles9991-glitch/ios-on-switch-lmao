// SPDX-License-Identifier: MIT
// A pack of runtime records (record.h, compute_record.h) laid out as directlink.py lays out the generator's: each
// render program a heap slot of its own (programSlot), a record's extras after its blob's pairs, its restore pairs after
// its argument tables, its stride twin a record of its own under the twin's keyHash, and a kernel's code at its slot's
// start (kernels.py lay_out). A host test opens gm20b::Device over it to draw a pack record of a state as the device draws
// its runtime record of the same state.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "../compute_record.h"
#include "../record.h"

namespace harness {

struct LaidRecords {
    std::vector<gm20b::PackProgram> programs;
    std::vector<uint8_t> heap;
    std::vector<gm20b::PackPipeline> pipelines;
    std::vector<gm20b::PackKernel> kernels;
    std::vector<uint32_t> blob;
    std::string strings = std::string(1, '\0');

    uint32_t program(const gm20b::CapturedProgram &p)
    {
        gm20b::PackProgram info;
        const std::vector<uint8_t> slot = gm20b::programSlot(p, info);
        info.heapOffset = uint32_t(heap.size());
        info.program += info.heapOffset;
        heap.insert(heap.end(), slot.begin(), slot.end());
        programs.push_back(info);
        return uint32_t(programs.size() - 1);
    }

    void kernel(uint64_t keyHash, const gm20b::ComputeRecord &r)
    {
        gm20b::PackProgram info = {};
        info.heapOffset = info.program = uint32_t(heap.size());
        heap.insert(heap.end(), r.code.begin(), r.code.end());
        heap.resize((heap.size() + 0xFF) & ~size_t(0xFF));
        info.dataOffset = r.data.empty() ? 0 : uint32_t(heap.size()) - info.heapOffset;
        info.dataBytes = uint32_t(r.data.size());
        heap.insert(heap.end(), r.data.begin(), r.data.end());
        heap.resize((heap.size() + 0xFF) & ~size_t(0xFF));
        info.bytes = uint32_t(heap.size()) - info.heapOffset;
        info.cbufs = r.cbufs;
        memcpy(info.cbufType, r.cbufType, sizeof r.cbufType);
        programs.push_back(info);
        gm20b::PackKernel k = r.kernel;
        k.keyHash = keyHash;
        k.program = uint32_t(programs.size() - 1);
        kernels.push_back(k);
    }

    void record(const gm20b::RuntimeRecord &r)
    {
        gm20b::PackPipeline pp = r.pipeline;
        pp.vs = program(r.programs[0]);
        pp.fs = program(r.programs[1]);
        for (uint32_t k = 0; gm20b::tessellates(pp) && k < 2; k++)
            pp.tessPrograms[k] = program(r.tessPrograms[k]);
        pp.keyString = uint32_t(strings.size());
        strings += r.keyString + std::string(1, '\0');
        pp.blob = uint32_t(blob.size());
        blob.insert(blob.end(), r.blob.begin(), r.blob.end());
        for (const auto &[m, v] : r.extras)
            blob.insert(blob.end(), {m, v});
        pp.blobDwords = uint32_t(blob.size()) - pp.blob;
        pp.spillAt = uint32_t(blob.size());
        blob.insert(blob.end(), r.spill.begin(), r.spill.end());
        pp.restoreAt = uint32_t(blob.size());
        for (const auto &[m, v] : r.restore)
            blob.insert(blob.end(), {m, v});
        pp.restoreDwords = uint16_t(blob.size() - pp.restoreAt);
        if (r.strideTwin) {
            pp.strideTwin = r.strideTwin->pipeline.keyHash;
            record(*r.strideTwin);
        }
        pipelines.push_back(pp);
    }
};

// The pack file of `records` and of `kernels` (each under its keyHash), written to the test's temporary directory as `name`
inline std::string recordPack(const std::vector<gm20b::RuntimeRecord> &records, const char *name,
                              const std::vector<std::pair<uint64_t, const gm20b::ComputeRecord *>> &kernels = {})
{
    LaidRecords laid;
    for (const gm20b::RuntimeRecord &r : records)
        laid.record(r);
    for (const auto &[keyHash, k] : kernels)
        laid.kernel(keyHash, *k);
    std::sort(laid.kernels.begin(), laid.kernels.end(), [](const gm20b::PackKernel &a, const gm20b::PackKernel &b) {
        return a.keyHash < b.keyHash;
    });
    gm20b::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = gm20b::kPackVersion;
    h.programs = uint32_t(laid.programs.size());
    h.pipelines = uint32_t(laid.pipelines.size());
    std::vector<uint8_t> file((sizeof h + 255) & ~size_t(255));
    const auto put = [&](gm20b::PackSectionId id, const void *bytes, size_t n) {
        file.resize((file.size() + 255) & ~size_t(255));
        h.section[id] = {file.size(), n};
        file.insert(file.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + n);
    };
    put(gm20b::PackPrograms, laid.programs.data(), laid.programs.size() * sizeof(gm20b::PackProgram));
    put(gm20b::PackPipelines, laid.pipelines.data(), laid.pipelines.size() * sizeof(gm20b::PackPipeline));
    put(gm20b::PackKernels, laid.kernels.data(), laid.kernels.size() * sizeof(gm20b::PackKernel));
    put(gm20b::PackStrings, laid.strings.data(), laid.strings.size());
    put(gm20b::PackBlob, laid.blob.data(), laid.blob.size() * 4);
    put(gm20b::PackHeap, laid.heap.data(), laid.heap.size());
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

}  // namespace harness
