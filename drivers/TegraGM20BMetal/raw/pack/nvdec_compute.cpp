// SPDX-License-Identifier: MIT
// nvdec's compute class (MAXWELL_COMPUTE_B, subchannel 1): a dispatch's canonical record, as nvdec.h describes for draws. Its QMD
// (Mesa src/nouveau/headers/nvidia/classes/cla1c0qmd.h, QMDV01_07: what NAK's nak_fill_qmd writes for GM20B) is printed field
// by field, with its addresses replaced by what they hold: the program by its code, each constant buffer by its content.
#include "nvdec.h"

#include <cstring>

namespace nvdec {
namespace {

// MAXWELL_COMPUTE_B methods (clb1c0.h)
enum : uint32_t {
    kWaitForIdle = 0x0110,
    kInvalidateShaderCaches = 0x021c,
    kInvalidateTextureHeaderCacheNoWfi = 0x0244,
    kInvalidateSkedCaches = 0x0298,
    kSendPcasA = 0x02b4,
    kInvalidateTextureDataCacheNoWfi = 0x1288,
    kInvalidateSamplerCacheNoWfi = 0x1424,
    kTscPoolA = 0x155c,
    kTscPoolB = 0x1560,
    kTicPoolA = 0x1574,
    kTicPoolB = 0x1578,
    kProgramRegionA = 0x1608,
    kProgramRegionB = 0x160c,
};

uint32_t field(const uint32_t *q, unsigned lo, unsigned hi)
{
    uint64_t v = q[lo / 32];
    if (hi / 32 != lo / 32)
        v |= uint64_t(q[hi / 32]) << 32;
    const unsigned width = hi - lo + 1;
    return uint32_t((v >> (lo % 32)) & ((width == 32) ? 0xffffffffull : ((1ull << width) - 1)));
}

std::string hex64(uint64_t v)
{
    char b[24];
    snprintf(b, sizeof b, "%llx", (unsigned long long)v);
    return b;
}

}  // namespace

const ProgramInfo *Decoder::computeProgram(uint64_t at, std::string &id)
{
    if (computeByHead_.empty())
        for (const auto &kv : programs)
            if (kv.second.image.size() > 80 + kComputeHead)
                computeByHead_.emplace(fnv(kv.second.image.data() + 80, kComputeHead), &kv.second);
    uint8_t head[256];
    if (!read(at, head, sizeof head)) {
        id = "unmapped";
        return nullptr;
    }
    auto range = computeByHead_.equal_range(fnv(head, kComputeHead));
    for (auto it = range.first; it != range.second; ++it) {
        const ProgramInfo &p = *it->second;
        const size_t code = p.image.size() - 80;
        std::vector<uint8_t> b(code), data(p.data.size());
        read(at, b.data(), code);
        if (memcmp(b.data(), p.image.data() + 80, code))
            continue;
        if (!data.empty() && !(read(at + ((code + 0xff) & ~size_t(0xff)), data.data(), data.size()) && data == p.data))
            continue;
        id = p.id;
        return &p;
    }
    id = "unknown:" + hex64(fnv(head, sizeof head));
    return nullptr;
}

// The compute class's barriers carry the names nvdec gives the 3D class's
void Decoder::computeMethod(uint32_t mthd, uint32_t data)
{
    switch (mthd) {
    case kWaitForIdle:
        barrier("WFI");
        break;
    case kInvalidateShaderCaches:
        barrier("SHINV");
        if (data & 1)
            barrier("ICINV");
        if (data & 0x1000) {
            barrier("CBINV");
            constantStale_.clear();
        }
        break;
    case kInvalidateTextureHeaderCacheNoWfi:
        barrier("TICINV");
        break;
    case kInvalidateSamplerCacheNoWfi:
        barrier("TSCINV");
        break;
    case kInvalidateTextureDataCacheNoWfi:
        barrier("TEXINV");
        break;
    case kInvalidateSkedCaches:
        barrier("SKEDINV");
        break;
    case kSendPcasA:
        dispatch(uint64_t(data) << 8);
        break;
    default:
        break;
    }
}

// Whether a constant buffer the QMD `q` binds (the root table too: no LOAD_CONSTANT_BUFFER writes it) lies over bytes in
// constantStale_
bool Decoder::dispatchReadsStaleConstants(const uint32_t q[64])
{
    for (int i = 0; i < 8 && !constantStale_.empty(); i++) {
        if (!field(q, 640 + i, 640 + i))
            continue;
        const uint64_t addr = (uint64_t(field(q, 960 + 64 * i, 967 + 64 * i)) << 32) | q[29 + 2 * i];
        const uint32_t size = field(q, 975 + 64 * i, 991 + 64 * i);
        for (const auto &r : constantStale_)
            if (addr < r.second && r.first < addr + size)
                return true;
    }
    return false;
}

// SEND_PCAS_A: the QMD at `qmd`. The record names the work, not where it lies: the QMD's every field but the program offset
// and the constant buffer addresses (QMDV01_07_PROGRAM_OFFSET, CONSTANT_BUFFER_ADDR_*), then the program and what each valid
// constant buffer holds (cb0, NVK's root table: its cs words, set addresses and push words, as nvdec prints a draw's)
void Decoder::dispatch(uint64_t qmd)
{
    dispatches++;
    uint32_t q[64];
    read(qmd, q, sizeof q);
    if (onDispatch)
        onDispatch(*this, qmd);
    if (dispatchReadsStaleConstants(q)) {
        staleConstantReads++;
        barrier("CBSTALE");
    }
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    fprintf(out_, "DISPATCH grid=%u,%u,%u block=%u,%u,%u barriers=%s\n", field(q, 384, 415), field(q, 416, 431),
            field(q, 432, 447), field(q, 592, 607), field(q, 608, 623), field(q, 624, 639), bar.c_str());
    uint32_t masked[64];
    memcpy(masked, q, sizeof masked);
    masked[8] = 0;
    for (int i = 0; i < 8; i++) {
        masked[29 + 2 * i] = 0;
        masked[30 + 2 * i] &= ~0xffu;
    }
    fprintf(out_, "  qmd=%s smem=%x l1=%u regs=%u barriers=%u slm=%x/%x crs=%x sass=%x version=%u.%u\n",
            hex64(fnv(masked, sizeof masked)).c_str(), field(q, 544, 561), field(q, 669, 671), field(q, 1496, 1503),
            field(q, 1467, 1471), field(q, 1440, 1463), field(q, 1472, 1495), field(q, 1504, 1527),
            field(q, 1528, 1535), field(q, 580, 583), field(q, 576, 579));
    const uint64_t region = (uint64_t(reg_[1][kProgramRegionA >> 2] & 0xff) << 32) | reg_[1][kProgramRegionB >> 2];
    std::string prog;
    computeProgram(region + q[8], prog);
    fprintf(out_, "  prog=%s\n", prog.c_str());
    // set 0's handles whose sampler index is 0 read no sampler (a texture read: m2v's handle names none, and NVK hands
    // index 0 out like any other)
    Layout handles;
    layout_ = nullptr;
    if (field(q, 640, 640)) {
        const uint64_t root = (uint64_t(field(q, 960, 967)) << 32) | q[29];
        uint32_t set[4] = {};
        read(root + 0x38, set, sizeof set);
        const uint64_t setVa = (uint64_t(set[1]) << 32) | set[0];
        std::vector<uint32_t> w(std::min<uint32_t>(set[2], 0x1000) / 4);
        if (setVa && read(setVa, w.data(), w.size() * 4)) {
            handles.bytes = uint32_t(w.size() * 4);
            handles.ranges = {{0, handles.bytes}};
            for (uint32_t k = 0; k < w.size(); k++)
                if (w[k] && !(w[k] >> 20))
                    handles.textureOnly.insert(4 * k);
            layout_ = &handles;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (!field(q, 640 + i, 640 + i))
            continue;
        const uint64_t addr = (uint64_t(field(q, 960 + 64 * i, 967 + 64 * i)) << 32) | q[29 + 2 * i];
        const uint32_t size = field(q, 975 + 64 * i, 991 + 64 * i);
        if (i == 0) {
            uint8_t cs[0x18];
            read(addr, cs, sizeof cs);
            fprintf(out_, "  cb0 root cs=%s size=%x\n", hex64(fnv(cs, sizeof cs)).c_str(), size);
            for (uint32_t set = 0; set < 2; set++)
                fprintf(out_, "  cb0 root set%u=%s\n", set,
                        contentHash(addr + 0x38 + 16 * set, 16, 0, false, true).c_str());
            fprintf(out_, "  cb0 root push=%s\n", contentHash(addr + 0x700, 0x80, 1, false, false).c_str());
        } else {
            fprintf(out_, "  cb%d size=%x data=%s\n", i, size, contentHash(addr, size, 1, true, false).c_str());
        }
    }
    layout_ = nullptr;
    // after the records above: the probe's scrub (NVDEC_SCRUB) zeroes the set and root they read
    static const bool probe = getenv("NVDEC_KERNELS") != nullptr;
    if (probe)
        probeKernel(q, prog);
}

// NVDEC_KERNELS: what a probe dispatch shows of its kernel (raw_compute.h KernelRecord), for the pack's generator: the QMD, the
// program, and set 0's entries, found by the probe's tags (each buffer slot's bytes start 0xC0DE0000 | slot; the range a
// descriptor gives a 4096-byte payload is the function's window on it)
void Decoder::probeKernel(const uint32_t *q, const std::string &prog)
{
    std::string line = "K qmd=";
    for (int i = 0; i < 64; i++) {
        char w[12];
        snprintf(w, sizeof w, "%s%x", i ? "," : "", q[i]);
        line += w;
    }
    fprintf(out_, "%s prog=%s\n", line.c_str(), prog.c_str());
    if (!field(q, 640, 640))
        return;
    const uint64_t root = (uint64_t(field(q, 960, 967)) << 32) | q[29];
    uint32_t r[0x800 / 4];
    read(root, r, sizeof r);
    std::string push = "K push";
    for (int k = 0; k < 32; k++)
        if (r[0x700 / 4 + k]) {
            char w[24];
            snprintf(w, sizeof w, " %d=%x", k, r[0x700 / 4 + k]);
            push += w;
        }
    fprintf(out_, "%s\n", push.c_str());
    // m2v's DispatchRegion push field (metal2vulkan lower/dispatch.rs region_bytes): base 0, the nominal threadgroup size 10
    // bits an axis (size - 1), the grid in threads, 0; the grid's threadgroups are the QMD's
    const uint32_t *pw = r + 0x700 / 4, raster[3] = {field(q, 384, 415), field(q, 416, 431), field(q, 432, 447)};
    for (int k = 0; k + 8 <= 32; k++) {
        bool region = !pw[k] && !pw[k + 1] && !pw[k + 2] && pw[k + 3] && !pw[k + 7];
        for (int i = 0; region && i < 3; i++)
            region = pw[k + 4 + i] == (((pw[k + 3] >> (10 * i)) & 0x3ff) + 1) * raster[i];
        if (region) {
            fprintf(out_, "K region %d\n", k);
            break;
        }
    }
    const uint64_t set = (uint64_t(r[0x38 / 4 + 1]) << 32) | r[0x38 / 4];
    const uint32_t setBytes = r[0x38 / 4 + 2];
    fprintf(out_, "K set %x\n", setBytes);
    if (!set || setBytes > 0x1000)
        return;
    std::vector<uint32_t> w(setBytes / 4);
    read(set, w.data(), setBytes);
    // m2v writes only its layout's words of a set (NVDEC_SCRUB, the probe's): zeroed now, the next dispatch's set holds no
    // word of this one's that its layout leaves alone
    static const bool scrubbing = getenv("NVDEC_SCRUB") != nullptr;
    if (scrub && scrubbing) {
        scrub(set, setBytes);
        scrub(root, sizeof r);
    }
    // the set's words: a buffer descriptor (an address of tagged bytes, its range), else a texture and sampler handle (tic |
    // tsc << 20) by the header each names: a texture's size and a sampler's words
    const uint64_t ticPool = (uint64_t(reg_[1][kTicPoolA >> 2] & 0xff) << 32) | reg_[1][kTicPoolB >> 2];
    const uint64_t tscPool = (uint64_t(reg_[1][kTscPoolA >> 2] & 0xff) << 32) | reg_[1][kTscPoolB >> 2];
    for (uint32_t k = 0; k < w.size(); k++) {
        if (k % 4 == 0 && k + 4 <= w.size()) {
            const uint64_t va = (uint64_t(w[k + 1]) << 32) | w[k];
            uint32_t tag = 0;
            if (va >> 32 && read(va, &tag, 4) && (tag & 0xffff0000u) == 0xc0de0000u) {
                fprintf(out_, "K entry %u buffer %u %x\n", k * 4, tag & 0xff, w[k + 2]);
                k += 3;
                continue;
            }
        }
        if (!w[k])
            continue;
        const uint32_t tic = w[k] & 0xfffff, tsc = w[k] >> 20;
        uint32_t t[8] = {}, s[8] = {};
        if (tic && read(ticPool + uint64_t(tic) * 32, t, sizeof t))
            fprintf(out_, "K entry %u texture %u %u %x\n", k * 4, (t[4] & 0xffff) + 1, (t[5] & 0xffff) + 1, t[0]);
        if (tsc && read(tscPool + uint64_t(tsc) * 32, s, sizeof s))
            fprintf(out_, "K entry %u sampler %x %x %x %x %x %x %x %x\n", k * 4, s[0], s[1], s[2], s[3], s[4], s[5],
                    s[6], s[7]);
    }
}

}  // namespace nvdec
