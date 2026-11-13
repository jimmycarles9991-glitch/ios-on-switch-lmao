// SPDX-License-Identifier: MIT
// What raw::Device submits, decoded by nvdec as raw/pack's mock kernel (mock_iokit.c, cap.cpp) decodes NVK's
// submissions for nvkframe: the kernel model (raw_kernel_model.h, whose IOKit calls replace the framework's) answers the
// device, every job it takes is decoded against the model's memory as the device left it, and completes. A binary
// links this unit to draw raw's side of eq (test/eq): RAWCAP_LOG, RAWCAP_LAYOUTS and NVDEC_REGS as cap.cpp reads them,
// and RAWCAP_PROGRAMS a colon-separated list (the pack's own programs.bin names raw's programs and their constant buffers).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "../../raw/pack/nvdec.h"
#include "../raw_kernel_model.h"

namespace {

class Capture {
public:
    Capture()
    {
        using kernel_model::model;
        // the program window's quarter of the client VA, and every address under 0x80'0000'0000: nvdec follows a
        // descriptor's address into what it names only there, as in the mock kernel's VA where NVK's side was recorded
        model.vaEnd = uint64_t(1) << 39;
        if (const char *path = getenv("RAWCAP_LOG"))
            log_ = fopen(path, "w");
        model.bound = [this](const TegraGM20BAbiBindOp &op) {
            if (op.kind)
                kinds_[op.va] = {op.range, op.kind};
            else
                kinds_.erase(op.va);
        };
        model.jobAdded = [this] {
            const kernel_model::Job &job = model.jobs.back();
            decode(job);
            if (!model.inFlight)
                model.fence(job.channel - 1).completed = job.seqno;
        };
    }

    uint32_t kind(uint64_t va) const
    {
        auto at = kinds_.upper_bound(va);
        if (at == kinds_.begin())
            return 0;
        --at;
        return va - at->first < at->second.first ? at->second.second : 0;
    }

private:
    static std::vector<std::string> split(const char *paths)
    {
        std::vector<std::string> out;
        for (const char *at = paths; *at;) {
            const char *end = strchr(at, ':');
            out.emplace_back(at, end ? end - at : strlen(at));
            at = end ? end + 1 : at + strlen(at);
        }
        return out;
    }

    nvdec::Decoder &decoder(uint32_t channel)
    {
        auto &d = decoders_[channel];
        if (!d) {
            d = std::make_unique<nvdec::Decoder>(
                [](uint64_t va, uint64_t bytes) -> const uint8_t * {
                    return kernel_model::model.host(va, bytes);
                },
                log_);
            d->vaKind = [this](uint64_t va) {
                return kind(va);
            };
            if (const char *paths = getenv("RAWCAP_PROGRAMS"))
                for (const std::string &path : split(paths))
                    if (!d->loadPrograms(path.c_str()))
                        fprintf(stderr, "eq_capture: cannot read programs %s\n", path.c_str());
            if (const char *path = getenv("RAWCAP_LAYOUTS"); path && !d->loadLayouts(path))
                fprintf(stderr, "eq_capture: cannot read layouts %s\n", path);
            if (getenv("EQ_DUMP_DRAWS"))
                d->onDraw = [this](nvdec::Decoder &at) {
                    dumpBindings(at);
                };
        }
        return *d;
    }

    // EQ_DUMP_DRAWS=<i>,<j>...: those draws' bound constant buffers of the vertex and pixel groups, and the buffers their
    // {address, range} descriptors name, word by word, to stderr
    void dumpBindings(nvdec::Decoder &d)
    {
        const unsigned draw = draws_++;
        const std::string wanted = "," + std::string(getenv("EQ_DUMP_DRAWS")) + ",";
        if (wanted.find("," + std::to_string(draw) + ",") == std::string::npos)
            return;
        for (const int group : {0, 4})
            for (int slot = 0; slot < 18; slot++) {
                uint64_t addr = 0;
                uint32_t bytes = 0;
                if (!d.bound(group, slot, addr, bytes))
                    continue;
                const std::vector<uint32_t> words =
                    dump(d, draw, "group " + std::to_string(group) + " cb" + std::to_string(slot), addr,
                         std::min<uint32_t>(bytes, 0x800));
                for (size_t k = 0; k + 4 <= words.size(); k += 4) {
                    const uint64_t va = words[k] | uint64_t(words[k + 1]) << 32;
                    if (words[k + 3] == 0 && words[k + 2] && words[k + 2] <= 0x10000 && kernel_model::model.host(va))
                        dump(d, draw, "  @" + std::to_string(k * 4), va, std::min<uint32_t>(words[k + 2], 0x800));
                }
            }
    }

    static std::vector<uint32_t> dump(nvdec::Decoder &d, unsigned draw, const std::string &what, uint64_t va,
                                      uint32_t bytes)
    {
        std::vector<uint32_t> words(bytes / 4);
        d.gpuRead(va, words.data(), words.size() * 4);
        fprintf(stderr, "draw %u %s va %llx size %x:", draw, what.c_str(), (unsigned long long)va, bytes);
        for (const uint32_t w : words)
            fprintf(stderr, " %08x", w);
        fprintf(stderr, "\n");
        return words;
    }

    void decode(const kernel_model::Job &job)
    {
        if (!log_)
            return;
        nvdec::Decoder &d = decoder(job.channel);
        d.submitBegin();
        for (const TegraGM20BAbiPush &push : job.pushes)
            if (const uint8_t *words = kernel_model::model.host(push.va, uint64_t(push.dwords) * 4))
                d.push(reinterpret_cast<const uint32_t *>(words), push.dwords);
        d.submitEnd();
        fflush(log_);
    }

    FILE *log_ = nullptr;
    unsigned draws_ = 0;
    std::map<uint32_t, std::unique_ptr<nvdec::Decoder>> decoders_;
    std::map<uint64_t, std::pair<uint64_t, uint32_t>> kinds_;  // va: the bind's range and PTE kind
};

Capture capture;

}  // namespace

extern "C" uint32_t mock_iokit_va_kind(uint64_t va)
{
    return capture.kind(va);
}

extern "C" void pushcount_report(const char *)
{}

extern "C" void mock_iokit_comp_stats(unsigned long out[5])
{
    for (int i = 0; i < 5; i++)
        out[i] = 0;
}
