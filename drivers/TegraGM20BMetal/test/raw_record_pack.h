// SPDX-License-Identifier: MIT
// A pack of runtime records (raw_record.h, raw_compute_record.h) laid out as directlink.py lays out the generator's: each
// render program a heap slot of its own (programSlot), a record's extras after its blob's pairs, its restore pairs after
// its argument tables, its stride twin a record of its own under the twin's keyHash, and a kernel's code at its slot's
// start (kernels.py lay_out). A host test opens raw::Device over it to draw a pack record of a state as the device draws
// its runtime record of the same state.
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "../raw/runtime/raw_compute_record.h"
#include "../raw/runtime/raw_record.h"

namespace harness {

struct LaidRecords {
    std::vector<raw::PackProgram> programs;
    std::vector<uint8_t> heap;
    std::vector<raw::PackPipeline> pipelines;
    std::vector<raw::PackKernel> kernels;
    std::vector<uint32_t> blob;
    std::string strings = std::string(1, '\0');

    uint32_t program(const raw::CapturedProgram &p)
    {
        raw::PackProgram info;
        const std::vector<uint8_t> slot = raw::programSlot(p, info);
        info.heapOffset = uint32_t(heap.size());
        info.program += info.heapOffset;
        heap.insert(heap.end(), slot.begin(), slot.end());
        programs.push_back(info);
        return uint32_t(programs.size() - 1);
    }

    void kernel(uint64_t keyHash, const raw::ComputeRecord &r)
    {
        raw::PackProgram info = {};
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
        raw::PackKernel k = r.kernel;
        k.keyHash = keyHash;
        k.program = uint32_t(programs.size() - 1);
        kernels.push_back(k);
    }

    void record(const raw::RuntimeRecord &r)
    {
        raw::PackPipeline pp = r.pipeline;
        pp.vs = program(r.programs[0]);
        pp.fs = program(r.programs[1]);
        for (uint32_t k = 0; raw::tessellates(pp) && k < 2; k++)
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
inline std::string recordPack(const std::vector<raw::RuntimeRecord> &records, const char *name,
                              const std::vector<std::pair<uint64_t, const raw::ComputeRecord *>> &kernels = {})
{
    LaidRecords laid;
    for (const raw::RuntimeRecord &r : records)
        laid.record(r);
    for (const auto &[keyHash, k] : kernels)
        laid.kernel(keyHash, *k);
    std::sort(laid.kernels.begin(), laid.kernels.end(), [](const raw::PackKernel &a, const raw::PackKernel &b) {
        return a.keyHash < b.keyHash;
    });
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    h.programs = uint32_t(laid.programs.size());
    h.pipelines = uint32_t(laid.pipelines.size());
    std::vector<uint8_t> file((sizeof h + 255) & ~size_t(255));
    const auto put = [&](raw::PackSectionId id, const void *bytes, size_t n) {
        file.resize((file.size() + 255) & ~size_t(255));
        h.section[id] = {file.size(), n};
        file.insert(file.end(), static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + n);
    };
    put(raw::PackPrograms, laid.programs.data(), laid.programs.size() * sizeof(raw::PackProgram));
    put(raw::PackPipelines, laid.pipelines.data(), laid.pipelines.size() * sizeof(raw::PackPipeline));
    put(raw::PackKernels, laid.kernels.data(), laid.kernels.size() * sizeof(raw::PackKernel));
    put(raw::PackStrings, laid.strings.data(), laid.strings.size());
    put(raw::PackBlob, laid.blob.data(), laid.blob.size() * 4);
    put(raw::PackHeap, laid.heap.data(), laid.heap.size());
    memcpy(file.data(), &h, sizeof h);
    const char *dir = getenv("TEST_TMPDIR");
    const std::string path = std::string(dir ? dir : "/tmp") + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return path;
}

}  // namespace harness
