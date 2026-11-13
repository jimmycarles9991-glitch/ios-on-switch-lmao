// SPDX-License-Identifier: MIT
// computeframe's raw and pack modes: the kernel of an NVK run's record (its K lines and NVK's program), or the pack's
// kernel of a raw_compute_key.h key, dispatched through the raw path's compute encoder (runtime/raw_compute.h)
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "computeframe.h"
#include "../runtime/raw_compute.h"
#include "../runtime/raw_compute_key.h"

extern "C" void pushcount_report(const char *what);

namespace {

using namespace computeframe;

uint64_t fnv(const uint8_t *p, size_t n, uint64_t h = 0xcbf29ce484222325ull)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// nvdec knows a compute program by its code's first kNvdecComputeHead bytes (pack/nvdec.h kComputeHead): a shorter one is
// unknown:<hash of the memory after it> in the log
constexpr uint32_t kNvdecComputeHead = 64;

// PackKernelEntry.slot of a constexpr sampler (pack/kernels.py CONSTEXPR)
constexpr uint32_t kConstexprSlot = 0x80;

// The program named `id` in programs.bin (pack/mkprograms.py: 'NVPG', code, cbuf count, data, cbuf types, 80-byte header,
// code, data; nvdec names it by the FNV-1a of header + code, then data). An unknown id takes the one kernel (a zero header)
// shorter than nvdec's head the file holds, as a kernel NAK compiles to its exit alone is.
bool program(const char *path, const std::string &id, raw::ComputeRecord &r)
{
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const uint8_t zeros[80] = {};
    const bool unknown = id.rfind("unknown:", 0) == 0;
    uint32_t taken = 0;
    for (size_t at = 0; at + 16 <= b.size();) {
        uint32_t code, count, data;
        memcpy(&code, &b[at + 4], 4);
        memcpy(&count, &b[at + 8], 4);
        memcpy(&data, &b[at + 12], 4);
        const uint8_t *types = &b[at + 16], *image = types + count, *bytes = image + 80 + code;
        char name[24];
        snprintf(name, sizeof name, "%llx", (unsigned long long)fnv(bytes, data, fnv(image, 80 + code)));
        const bool shortKernel = unknown && code < kNvdecComputeHead && !memcmp(image, zeros, sizeof zeros);
        if (id == name || shortKernel) {
            r.code.assign(image + 80, image + 80 + code);
            r.data.assign(bytes, bytes + data);
            for (uint32_t c = 0; c < count && c < 8; c++)
                r.cbufType[c] = types[c];
            if (!unknown)
                return true;
            taken++;
        }
        at += 16 + count + 80 + code + data;
    }
    return taken == 1;
}

// The first dispatch's K lines of an NVDEC_KERNELS log (pack/nvdec_compute.cpp), read as pack/mkpack.py reads them
bool record(const char *log, const char *programs, raw::ComputeRecord &r)
{
    std::ifstream in(log);
    std::string line, prog;
    bool qmd = false;
    std::map<int, uint32_t> push;
    raw::PackKernel &k = r.kernel;
    k.regionWord = -1;
    auto entry = [&](uint32_t offset, uint8_t kind, uint32_t slot, uint32_t window) {
        if (k.entries < 16)
            k.entry[k.entries++] = {uint16_t(offset), kind, uint8_t(slot), window};
    };
    while (std::getline(in, line)) {
        unsigned offset, slot, bytes, w, h, t[8];
        if (!qmd && line.rfind("K qmd=", 0) == 0) {
            std::istringstream words(line.substr(6, line.find(' ', 6) - 6));
            std::string word;
            for (uint32_t i = 0; i < raw::kQmdWords && std::getline(words, word, ','); i++)
                k.qmd[i] = uint32_t(strtoul(word.c_str(), nullptr, 16));
            prog = line.substr(line.find(" prog=") + 6);
            qmd = true;
        } else if (qmd && line.rfind("K push", 0) == 0) {
            std::istringstream words(line.substr(6));
            std::string word;
            while (words >> word)
                push[atoi(word.c_str())] = uint32_t(strtoul(word.c_str() + word.find('=') + 1, nullptr, 16));
        } else if (qmd && sscanf(line.c_str(), "K region %u", &slot) == 1) {
            k.regionWord = int8_t(slot);
        } else if (qmd && sscanf(line.c_str(), "K set %x", &bytes) == 1) {
            k.setBytes = uint16_t(bytes);
        } else if (qmd && sscanf(line.c_str(), "K entry %u buffer %u %x", &offset, &slot, &bytes) == 3) {
            entry(offset, raw::KernelBuffer, slot, bytes < kTagBytes ? bytes : 0);
        } else if (qmd && sscanf(line.c_str(), "K entry %u texture %u %u", &offset, &w, &h) == 3) {
            if (w < 16 || h != w - 8)
                return false;
            entry(offset, raw::KernelTexture, w - 16, 0);
        } else if (qmd && sscanf(line.c_str(), "K entry %u sampler %x %x %x %x %x %x %x %x", &offset, &t[0], &t[1],
                                 &t[2], &t[3], &t[4], &t[5], &t[6], &t[7]) == 9) {
            // max LOD clamp, 4.8 fixed: the probe's tag 1 + slot / 2; any other is the kernel's own constexpr sampler, whose
            // header the record carries (raw::ComputeRecord::samplers, slot 0x80 | its index), as the pack's kernels do
            const uint32_t lod = (t[2] >> 12) & 0xfff;
            if (lod >= 256 && lod % 128 == 0) {
                entry(offset, raw::KernelSampler, (lod - 256) / 128, 0);
            } else {
                r.samplers.push_back({t[0], t[1], t[2], t[3], t[4], t[5], t[6], t[7]});
                entry(offset, raw::KernelSampler, kConstexprSlot | uint32_t(r.samplers.size() - 1), 0);
            }
        } else if (qmd && line.rfind("DISPATCH", 0) == 0) {
            break;
        }
    }
    for (const auto &[word, value] : push)
        if (k.regionWord < 0 || word < k.regionWord || word >= k.regionWord + 8) {
            k.pushMask |= 1u << word;
            k.pushWords[word] = value;
        }
    return qmd && program(programs, prog, r);
}

}  // namespace

int computeframeRaw(char **argv)
{
    uint32_t threads[3], groups[3];
    if (!size3(argv[5], threads) || !size3(argv[6], groups))
        return 2;
    raw::Device dev;
    std::string err;
    if (!dev.open(argv[2], 0, err)) {
        fprintf(stderr, "NO DEVICE %s\n", err.c_str());
        return 1;
    }
    // the pack's kernel of key argv[4] (pack mode), or the record of an NVK run's log
    raw::ComputeRecord rec;
    const bool packed = !strcmp(argv[1], "pack");
    if (!packed && !record(argv[3], argv[4], rec)) {
        fprintf(stderr, "no kernel record in %s\n", argv[3]);
        return 1;
    }
    const raw::ComputeState *k =
        packed ? raw::computeState(dev, raw::computeKeyHash(argv[4])) : raw::placeCompute(dev, rec);
    raw::CommandBuffer *cb = dev.begin();
    if (!k || !cb) {
        fprintf(stderr, "no kernel or command buffer\n");
        return 1;
    }
    raw::ComputeEncoder enc(*cb, dev);
    enc.setState(k);
    for (uint32_t s = 0; s < kSlots; s++)
        enc.setBytes(s, tag(s).data(), kTagBytes);
    raw::Buffer big;
    if (const uint32_t bytes = bigBuffer()) {
        if (!dev.createBuffer(bytes, big))
            return 1;
        fillBig(big.cpu, bytes);
        enc.setBuffer(kBigSlot, &big, 0);
    }
    raw::Buffer shifted;
    if (uint32_t slot, offset; off16(slot, offset)) {
        if (!dev.createBuffer(offset + kTagBytes, shifted))
            return 1;
        memset(shifted.cpu, 0, offset);
        memcpy(shifted.cpu + offset, tag(slot).data(), kTagBytes);
        enc.setBuffer(slot, &shifted, offset);
    }
    std::vector<raw::Texture> textures(kTextures);
    std::vector<raw::Sampler> samplers(kTextures);
    for (uint32_t t = 0; t < kTextures; t++) {
        const char *format = getenv("FRAME_TEXTURE_FORMAT");
        if (!dev.createTexture(16 + t, 8 + t, format ? uint32_t(atoi(format)) : kRgba8Unorm, textures[t], false,
                               getenv("FRAME_TILED") != nullptr))
            return 1;
        if (textures[t].cpu && !textures[t].blockLinear) {
            const std::vector<uint32_t> v = texels(t);
            for (uint32_t y = 0; y < 8 + t; y++)
                memcpy(textures[t].cpu + y * textures[t].pitch, &v[y * (16 + t)], 4 * (16 + t));
            cb->clean(textures[t].cpu, uint64_t(textures[t].pitch) * (8 + t));
        }
        raw::SamplerDesc sd;
        sd.lodMax = 1 + t / 2.0f;
        if (!dev.createSampler(sd, samplers[t]))
            return 1;
        enc.setTexture(t, &textures[t]);
        enc.setSampler(t, &samplers[t]);
    }
    const char *why = nullptr;
    enc.setSerial(true);
    for (uint32_t n = 0; n < dispatches(); n++)
        if (!enc.dispatch(groups, threads, &why)) {
            fprintf(stderr, "refused: %s\n", why);
            return 1;
        }
    enc.end();
    uint32_t seq = 0;
    if (!cb->commit(seq) || !dev.wait(seq))
        return 1;
    pushcount_report("computeframe raw");
    fprintf(stderr, "raw: %llu dispatches, %llu programs placed\n", (unsigned long long)dev.stats.dispatches.load(),
            (unsigned long long)dev.stats.programsPlaced.load());
    return 0;
}
