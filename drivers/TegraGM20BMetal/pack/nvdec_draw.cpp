// SPDX-License-Identifier: MIT
#include "nvdec.h"
#include "nvdec_methods.h"

#include <algorithm>
#include <cstring>

namespace nvdec {
namespace {

// Registers that are not state a draw reads, or that only mean something as an address, or that NVK uses as its own
// bookkeeping: never compared as plain values.
bool ignored3d(uint32_t m)
{
    if (m < 0x0200)
        return true;  // control, MME RAM loads, WFI
    if (m == INVALIDATE_SHADER_CACHES || m == INVALIDATE_SHADER_CACHES_NO_WFI || m == INVALIDATE_TEXTURE_DATA_CACHE ||
        m == INVALIDATE_TEXTURE_DATA_CACHE_NO_WFI || m == INVALIDATE_SAMPLER_CACHE_NO_WFI ||
        m == INVALIDATE_TEXTURE_HEADER_CACHE_NO_WFI || m == FLUSH_PENDING_WRITES || m == TILED_CACHE_BARRIER ||
        m == TILED_CACHE_FLUSH)
        return true;
    if (m >= CLEAR_RECT_H && m <= COLOR_CLEAR_VALUE + 0xc)
        return true;  // clear rect/vertex array triggers/clear values
    if (m == CLEAR_SURFACE_CONTROL || m == CLEAR_SURFACE || m == RENDER_ENABLE_OVERRIDE)
        return true;
    if (m == BEGIN || m == END)
        return true;
    if (m >= REPORT_SEMAPHORE_A && m <= REPORT_SEMAPHORE_D)
        return true;
    if (m >= CB_SELECTOR_A && m < 0x2600)
        return true;  // cbuf selector/loads/binds (compared as cbufs)
    if (m >= MME_SCRATCH)
        return true;  // MME shadow scratch, macro calls
    // addresses (printed by name or by content instead)
    if (m >= COLOR_TARGET && m < COLOR_TARGET + 0x200 && ((m & 0x3f) == 0 || (m & 0x3f) == 4))
        return true;
    if (m == ZT_A || m == ZT_B || m == PROGRAM_REGION_A || m == PROGRAM_REGION_B)
        return true;
    if (m == TIC_POOL_A || m == TIC_POOL_B || m == TSC_POOL_A || m == TSC_POOL_B)
        return true;
    if (m >= VERTEX_STREAM_A && m < VERTEX_STREAM_A + 0x200 && ((m & 0xf) == 4 || (m & 0xf) == 8))
        return true;
    if (m >= VERTEX_STREAM_LIMIT && m < VERTEX_STREAM_LIMIT + 0x100)
        return true;
    if (m >= PIPELINE && m < PIPELINE + 0x180 && (m & 0x3f) == 4)
        return true;  // program offsets: compared as programs
    // other address-bearing state NVK sets once (shader local memory, zcull, query/ICB, index buffer, MME DMA), and an
    // indexed draw's first index and count (SET_INDEX_BUFFER_F, DRAW_INDEX_BUFFER: its ib line's)
    static const uint32_t addrs[] = {0x0790, 0x0794, 0x07e0, 0x07e4, 0x07e8, 0x07ec, 0x07f0,
                                     0x07f4, 0x07f8, 0x07fc, 0x0f84, 0x0f88, 0x1550, 0x1554,
                                     0x17c8, 0x17cc, 0x17d0, 0x17d4, 0x17d8, 0x17dc, 0x17e0};
    return std::find(std::begin(addrs), std::end(addrs), m) != std::end(addrs);
}
}  // namespace

void Decoder::draw(const char *kind, uint32_t first, uint32_t count)
{
    draws++;
    if (onDraw)
        onDraw(*this);
    if (!constantStale_.empty() && readsStaleConstants()) {
        staleConstantReads++;
        barrier("CBSTALE");
    }
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    fprintf(out_, "%s first=%u count=%u begin=%08x barriers=%s\n", indexed_ ? "DRAW-INDEXED" : kind, first, count,
            r3(BEGIN), bar.c_str());
    countComp(0);

    // Which render targets, vertex streams and stages are live: their registers are compared, the others are not.
    const uint32_t ct = r3(CT_SELECT), ctCount = ct & 0xf;
    bool rtLive[8] = {};
    for (uint32_t k = 0; k < ctCount && k < 8; k++)
        rtLive[(ct >> (4 + 3 * k)) & 7] = true;
    bool stageLive[6] = {};
    for (int i = 0; i < 6; i++)
        stageLive[i] = r3(PIPELINE + 0x40 * i) & 1;
    auto live = [&](uint32_t m) {
        if (m >= COLOR_TARGET && m < COLOR_TARGET + 0x200)
            return rtLive[(m - COLOR_TARGET) / 0x40];
        if (m >= COLOR_COMPRESSION && m < COLOR_COMPRESSION + 0x20)
            return rtLive[(m - COLOR_COMPRESSION) / 4];
        if (m >= VERTEX_STREAM_A && m < VERTEX_STREAM_A + 0x200) {
            const uint32_t j = (m - VERTEX_STREAM_A) / 0x10;
            return bool(r3(VERTEX_STREAM_A + 0x10 * j) & (1u << 12));
        }
        if (m >= 0x1880 && m < 0x18c0)
            return bool(r3(VERTEX_STREAM_A + 0x10 * ((m - 0x1880) / 4)) & (1u << 12));
        if (m >= PIPELINE && m < PIPELINE + 0x180) {
            const uint32_t i = (m - PIPELINE) / 0x40;
            return (m & 0x3f) == 0 || stageLive[i];
        }
        // the primitive restart index is the index buffer's: a vertex array draw reads none (NVK leaves its last index
        // buffer's in place or puts its pipeline's 0 back, as its dynamic state goes, before either)
        if (m == 0x1648)
            return indexed_;
        return true;
    };

    if (firstDraw_ && fullStateAtFirstDraw) {
        for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
            const uint32_t m = i * 4;
            if (!ignored3d(m) && live(m) && reg_[0][i])
                fprintf(out_, "  STATE %04x=%08x\n", m, reg_[0][i]);
        }
    }
    firstDraw_ = false;

    if (getenv("NVDEC_PROBE"))
        probeDump(first, count);
    // every live, comparable register (the record differs between two streams iff the GPU state they leave differs)
    std::string regs;
    uint64_t regHash = 0xcbf29ce484222325ull;
    for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
        const uint32_t m = i * 4;
        if (ignored3d(m) || !live(m) || !reg_[0][i])
            continue;
        regHash = fnv(&m, 4, regHash);
        regHash = fnv(&reg_[0][i], 4, regHash);
    }
    fprintf(out_, "  regs=%s\n", hex(regHash).c_str());
    if (getenv("NVDEC_REGS")) {  // every live comparable register, for tools/eqcheck.py to name what differs
        std::string l = "  R";
        for (uint32_t i = 0x200 / 4; i < 0x4000; i++) {
            const uint32_t m = i * 4;
            if (ignored3d(m) || !live(m) || !reg_[0][i])
                continue;
            char b[24];
            snprintf(b, sizeof b, " %x=%x", m, reg_[0][i]);
            l += b;
        }
        fprintf(out_, "%s\n", l.c_str());
        // sampler header 0, which a handle naming no sampler names: NVK's texel-fetch sampler on every GPU (nvk_device.c)
        fprintf(out_, "  tsc0=%s\n", tscEntry(0).c_str());
    }
    // render targets and depth target by name
    for (uint32_t j = 0; j < 8; j++) {
        if (!rtLive[j])
            continue;
        const uint32_t m = COLOR_TARGET + 0x40 * j;
        fprintf(out_, "  rt%u=%s %ux%u fmt=%x mem=%x pitch=%x\n", j, name(addr3(m, m + 4)).c_str(), r3(m + 8),
                r3(m + 0xc), r3(m + 0x10), r3(m + 0x14), r3(m + 0x1c));
    }
    if (r3(ZT_SELECT) & 1)
        fprintf(out_, "  zt=%s\n", name(addr3(ZT_A, ZT_B)).c_str());
    // vertex streams: the bytes the draw reads
    for (uint32_t j = 0; j < 16; j++) {
        const uint32_t fmt = r3(VERTEX_STREAM_A + 0x10 * j);
        if (!(fmt & (1u << 12)))
            continue;
        const uint64_t loc = addr3(VERTEX_STREAM_A + 0x10 * j + 4, VERTEX_STREAM_A + 0x10 * j + 8);
        const uint64_t limit = addr3(VERTEX_STREAM_LIMIT + 8 * j, VERTEX_STREAM_LIMIT + 8 * j + 4);
        const uint32_t stride = fmt & 0xfff;
        uint64_t bytes = uint64_t(first + count) * stride;
        if (limit >= loc)
            bytes = std::min<uint64_t>(bytes, limit - loc + 1);
        uint32_t head[4] = {};
        read(loc, head, 16);
        if (describe && getenv("NVDEC_DESCRIBE"))
            describe(loc);
        fprintf(out_, "  vb%u stride=%u data=%s head=%08x,%08x,%08x,%08x\n", j, stride,
                contentHash(loc, bytes, 2, false, false).c_str(), head[0], head[1], head[2], head[3]);
    }
    // the draw's set-0 layout, by its program pair
    layout_ = nullptr;
    {
        std::string vs, fs;
        if (stageLive[1])
            program(1, vs);
        if (stageLive[5])
            program(5, fs);
        auto it = layouts.find(vs + " " + fs);
        if (it != layouts.end())
            layout_ = &it->second;
        else if (!layouts.empty())
            fprintf(out_, "  layout unknown\n");
    }
    // programs and the constant buffers each live stage's program uses
    for (int i = 0; i < 6; i++) {
        if (!stageLive[i])
            continue;
        std::string prog;
        const ProgramInfo *info = program(i, prog);
        const uint32_t group = r3(PIPELINE + 0x40 * i + 0x10);
        fprintf(out_, "  stage%d prog=%s regs=%u group=%u\n", i, prog.c_str(), r3(PIPELINE + 0x40 * i + 0xc), group);
        if (group >= 5)
            continue;
        const size_t slots = info ? info->cbufTypes.size() : 18;
        for (size_t s = 0; s < slots; s++) {
            const Bind &b = binds_[group][s];
            const uint8_t type = info ? info->cbufTypes[s] & 7 : 0;  // pack.h PackCbufType
            if (!b.valid) {
                if (info)
                    fprintf(out_, "    cb%zu INVALID\n", s);
                continue;
            }
            if (type == 1 || (type == 0 && s == 0)) {
                // root table: draw parameters, set addresses, push constants (nvk_root_descriptor_table)
                uint8_t draw[0x38];
                read(b.addr, draw, sizeof draw);
                // sample_locations[1..7] (NVK uploads 8 bytes of which a 1-sample layout sets one) and sample_masks[] (read only by
                // sample-rate shading; no Core Animation program has it, nak_shader_info.fs): not compared
                memset(draw + 0x1d, 0, 0x34 - 0x1d);
                fprintf(out_, "    cb%zu root draw=%s\n", s, hex(fnv(draw, sizeof draw)).c_str());
                for (uint32_t set = 0; set < 2; set++)
                    fprintf(out_, "    cb%zu root set%u=%s\n", s, set,
                            contentHash(b.addr + 0x38 + 16 * set, 16, 0, false, true).c_str());
                fprintf(out_, "    cb%zu root push=%s\n", s,
                        contentHash(b.addr + 0x700, 0x80, 1, false, false).c_str());
                if (getenv("NVDEC_ROOTWORDS")) {
                    uint32_t w[0x780 / 4];
                    read(b.addr, w, sizeof w);
                    std::string l = "    rootwords draw:";
                    for (int k = 0; k < 14; k++) {
                        char x[12];
                        snprintf(x, sizeof x, " %x", w[k]);
                        l += x;
                    }
                    l += " push:";
                    for (int k = 0x700 / 4; k < 0x780 / 4; k++) {
                        char x[12];
                        snprintf(x, sizeof x, " %x", w[k]);
                        l += x;
                    }
                    fprintf(out_, "%s\n", l.c_str());
                }
            } else if (type == kSplitFrontier) {
                const auto launch = splitLaunches_.find(b.addr);
                fprintf(out_, "    cb%zu frontier size=%x %s\n", s, b.size,
                        launch == splitLaunches_.end() ? "written by no prologue" : launch->second.c_str());
            } else {
                fprintf(out_, "    cb%zu type=%u size=%x data=%s\n", s, type, b.size,
                        contentHash(b.addr, b.size, type == 3 ? 1 : 2, type == 3, false).c_str());
            }
        }
    }
}

}  // namespace nvdec
