// SPDX-License-Identifier: MIT
#include "nvdec.h"
#include "nvdec_methods.h"

#include <algorithm>

namespace nvdec {

void Decoder::clear(uint32_t data)
{
    clears++;
    countComp(data);
    std::string bar;
    for (auto &b : pendingBarriers_)
        bar += (bar.empty() ? "" : ",") + b;
    pendingBarriers_.clear();
    const uint32_t rt = (data >> 6) & 0xf;
    const uint32_t m = COLOR_TARGET + 0x40 * rt;
    // depth (Z_ENABLE) or stencil (STENCIL_ENABLE): their clear values and the depth target too
    char zs[96] = "";
    if (data & 3)
        snprintf(zs, sizeof zs, " zs=%08x,%02x,%s", r3(Z_CLEAR_VALUE), r3(STENCIL_CLEAR_VALUE) & 0xff,
                 name(addr3(ZT_A, ZT_B)).c_str());
    fprintf(
        out_,
        "CLEAR data=%08x rt=%s,%ux%u,fmt=%x,mem=%x color=%08x,%08x,%08x,%08x rect=%08x,%08x ctl=%08x%s barriers=%s\n",
        data, name(addr3(m, m + 4)).c_str(), r3(m + 8), r3(m + 0xc), r3(m + 0x10), r3(m + 0x14), r3(COLOR_CLEAR_VALUE),
        r3(COLOR_CLEAR_VALUE + 4), r3(COLOR_CLEAR_VALUE + 8), r3(COLOR_CLEAR_VALUE + 0xc), r3(CLEAR_RECT_H),
        r3(CLEAR_RECT_V), r3(CLEAR_SURFACE_CONTROL), zs, bar.c_str());
}

// A draw (clearData 0) or a clear into a compressed target (nvdec_comp.h)
void Decoder::countComp(uint32_t clearData)
{
    if (!vaKind)
        return;
    const uint32_t rt = (clearData >> 6) & 0xf, m = COLOR_TARGET + 0x40 * rt;
    const uint32_t colorKind = r3(COLOR_COMPRESSION + 4 * rt) ? vaKind(addr3(m, m + 4)) : 0;
    const uint32_t zetaKind = (r3(ZT_SELECT) & 1) && r3(Z_COMPRESSION) ? vaKind(addr3(ZT_A, ZT_B)) : 0;
    auto add = [&](uint64_t CompCounts::*f, uint64_t n) {
        comp.*f += n;
        compSubmit.*f += n;
    };
    if (!clearData) {
        uint32_t live = r3(CT_SELECT) & 0xf;
        bool any = compressibleKind(zetaKind);
        for (uint32_t j = 0; j < live && j < 8 && !any; j++)
            any = r3(COLOR_COMPRESSION + 4 * j) &&
                  compressibleKind(vaKind(addr3(COLOR_TARGET + 0x40 * j, COLOR_TARGET + 0x40 * j + 4)));
        if (any)
            add(&CompCounts::draws, 1);
        return;
    }
    const bool rect = r3(CLEAR_SURFACE_CONTROL) & 0x10;
    const uint32_t h = r3(CLEAR_RECT_H), v = r3(CLEAR_RECT_V);
    auto whole = [&](uint32_t width, uint32_t height) {
        return !rect || (!(h & 0xffff) && !(v & 0xffff) && (h >> 16) >= width && (v >> 16) >= height);
    };
    if ((clearData & 0x3c) && compressibleKind(colorKind)) {
        const uint32_t w[4] = {r3(COLOR_CLEAR_VALUE), r3(COLOR_CLEAR_VALUE + 4), r3(COLOR_CLEAR_VALUE + 8),
                               r3(COLOR_CLEAR_VALUE + 0xc)};
        add(&CompCounts::clears, 1);
        if ((clearData & 0x3c) == 0x3c && whole(r3(m + 8), r3(m + 0xc)) && zbcBootColorNv(r3(m + 0x10), w)) {
            add(&CompCounts::zbc, 1);
            add(&CompCounts::zbcBytes, uint64_t(r3(m + 8)) * r3(m + 0xc) * compBytesPerTexel(colorKind));
        }
    }
    if ((clearData & 1) && compressibleKind(zetaKind)) {
        add(&CompCounts::clears, 1);
        if (whole(r3(ZT_SIZE_A), r3(ZT_SIZE_A + 4)) && zbcBootDepthBits(r3(Z_CLEAR_VALUE))) {
            add(&CompCounts::zbc, 1);
            add(&CompCounts::zbcBytes, uint64_t(r3(ZT_SIZE_A)) * r3(ZT_SIZE_A + 4) * compBytesPerTexel(zetaKind));
        }
    }
}

// Raw facts of a draw for the pack generator (tools/mkpack.py) and the runtime's record maker: every non-zero 3D register, the
// bound constant buffers, the root table's words, the sets' words, the words behind each pointer in them, the texture/sampler
// headers set 0 can name and the pools' first entries, the vertex streams and the programs.
void Decoder::probeRecord(ProbeRecord &r)
{
    r = {};
    for (uint32_t i = 0x200 / 4; i < 0x4000; i++)
        if (reg_[0][i] && i * 4 < MME_SCRATCH)
            r.regs[i * 4] = reg_[0][i];
    auto words = [&](uint64_t va, uint32_t bytes) {
        ProbeRecord::Words w{va, std::vector<uint32_t>(bytes / 4)};
        read(va, w.words.data(), bytes);
        return w;
    };
    auto pointer = [&](uint64_t v) {
        return (v >> 32) >= 1 && (v >> 32) < 0x80 && map_ && map_(v, 1);
    };
    for (int g = 0; g < 5; g++)
        for (int s = 0; s < 18; s++)
            if (binds_[g][s].valid)
                r.binds.push_back({g, s, binds_[g][s].addr, binds_[g][s].size});
    const uint64_t root = binds_[4][0].valid ? binds_[4][0].addr : binds_[0][0].addr;
    r.root = words(root, 0x780);
    // the root-table words written this submission in the push windows [0x700, 0x780): m2v's push ranges as NVK wrote them
    for (auto it = writtenThisSubmit_.lower_bound(root + 0x700); it != writtenThisSubmit_.end() && *it < root + 0x780;
         ++it)
        r.pushWords.push_back({uint32_t(*it - root), cbufWrites_[*it]});
    const std::vector<uint32_t> &rw = r.root.words;
    for (int set = 0; set < 2; set++) {
        const uint64_t sa = (uint64_t(rw[(0x38 + 16 * set) / 4 + 1]) << 32) | rw[(0x38 + 16 * set) / 4];
        const uint32_t ss = rw[(0x38 + 16 * set) / 4 + 2];
        if (!sa || !ss)
            continue;
        r.sets[set] = words(sa, std::min<uint32_t>(ss, 0x1000));
        const std::vector<uint32_t> &d = r.sets[set].words;
        for (size_t i = 0; i + 3 < d.size(); i += 2) {
            const uint64_t v = (uint64_t(d[i + 1]) << 32) | d[i];
            if (pointer(v))
                r.setPointers[set][uint32_t(i * 4)] = words(v, 256);
        }
    }
    for (uint32_t i = 0x700 / 4; i + 1 < 0x700 / 4 + 32; i += 2) {
        const uint64_t v = (uint64_t(rw[i + 1]) << 32) | rw[i];
        if (pointer(v))
            r.pushPointers[(i - 0x700 / 4) * 4] = words(v, 256);
    }
    // the texture and sampler headers any set-0 word could name (tic in bits 19:0, tsc in 31:20), plus entry 0 of each pool
    std::set<uint32_t> tics = {0}, tscs = {0};
    for (uint32_t w : r.sets[0].words) {
        if ((w & 0xfffff) <= r3(TIC_POOL_C))
            tics.insert(w & 0xfffff);
        if ((w >> 20) <= r3(TSC_POOL_C))
            tscs.insert(w >> 20);
    }
    auto header = [](const uint32_t (&e)[8]) {
        std::array<uint32_t, 8> a;
        std::copy(e, e + 8, a.begin());
        return a;
    };
    for (uint32_t t : tics) {
        uint32_t e[8];
        read(addr3(TIC_POOL_A, TIC_POOL_B) + 32ull * t, e, 32);
        if (std::any_of(e, e + 8, [](uint32_t x) {
                return x != 0;
            }))
            r.tic[t] = {name((uint64_t(e[2] & 0xffff) << 32) | (e[1] & 0xffffffe0u)), header(e)};
    }
    for (uint32_t t : tscs) {
        uint32_t e[8];
        read(addr3(TSC_POOL_A, TSC_POOL_B) + 32ull * t, e, 32);
        if (std::any_of(e, e + 8, [](uint32_t x) {
                return x != 0;
            }))
            r.tsc[t] = header(e);
    }
    for (uint32_t j = 0; j < 16; j++) {
        const uint32_t fmt = r3(VERTEX_STREAM_A + 0x10 * j);
        if (!(fmt & (1u << 12)))
            continue;
        ProbeRecord::Stream &s = r.streams[j];
        s.format = fmt;
        s.location = addr3(VERTEX_STREAM_A + 0x10 * j + 4, VERTEX_STREAM_A + 0x10 * j + 8);
        s.limit = addr3(VERTEX_STREAM_LIMIT + 8 * j, VERTEX_STREAM_LIMIT + 8 * j + 4);
        s.words = words(s.location, 256).words;
    }
    for (int i = 0; i < 6; i++) {
        if (!(r3(PIPELINE + 0x40 * i) & 1))
            continue;
        ProbeRecord::Program &p = r.programs[i];
        p.known = program(i, p.id) != nullptr;
        p.offset = r3(PIPELINE + 0x40 * i + 4);
    }
    r.begin = r3(BEGIN);
    const uint32_t ct = r3(CT_SELECT);
    for (uint32_t k = 0; k < (ct & 0xf) && k < 8; k++) {
        const uint32_t j = (ct >> (4 + 3 * k)) & 7, m = COLOR_TARGET + 0x40 * j;
        r.targets[name(addr3(m, m + 4))] = j;
    }
}

namespace {

void printWords(FILE *out, const std::string &tag, const ProbeRecord::Words &w)
{
    std::string l = "P " + tag + " " + hex(w.va);
    for (uint32_t x : w.words) {
        char b[12];
        snprintf(b, sizeof b, " %x", x);
        l += b;
    }
    fprintf(out, "%s\n", l.c_str());
}

void printProbe(FILE *out, const ProbeRecord &r)
{
    fprintf(out, "P begin\n");
    std::string line = "P regs";
    for (const auto &[m, v] : r.regs) {
        char b[24];
        snprintf(b, sizeof b, " %x=%x", m, v);
        line += b;
    }
    fprintf(out, "%s\n", line.c_str());
    for (const ProbeRecord::Bind &b : r.binds)
        fprintf(out, "P bind %d %d %s %x\n", b.group, b.slot, hex(b.addr).c_str(), b.bytes);
    printWords(out, "root", r.root);
    std::string l = "P pushw";
    for (const auto &[offset, word] : r.pushWords) {
        char b[32];
        snprintf(b, sizeof b, " %x=%x", offset, word);
        l += b;
    }
    fprintf(out, "%s\n", l.c_str());
    for (int set = 0; set < 2; set++) {
        if (r.sets[set].words.empty())
            continue;
        printWords(out, "set" + std::to_string(set), r.sets[set]);
        for (const auto &[offset, w] : r.setPointers[set])
            printWords(out, "ptr" + std::to_string(set) + "." + std::to_string(offset), w);
    }
    for (const auto &[offset, w] : r.pushPointers)
        printWords(out, "pushptr." + std::to_string(offset), w);
    for (const auto &[t, h] : r.tic)
        fprintf(out, "P tic %u %s %x %x %x %x %x %x %x %x\n", t, h.name.c_str(), h.w[0], h.w[1], h.w[2], h.w[3], h.w[4],
                h.w[5], h.w[6], h.w[7]);
    for (const auto &[t, e] : r.tsc)
        fprintf(out, "P tsc %u %x %x %x %x %x %x %x %x\n", t, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
    for (const auto &[j, s] : r.streams) {
        fprintf(out, "P vbinfo %u %s %s %x\n", j, hex(s.location).c_str(), hex(s.limit).c_str(), s.format);
        printWords(out, "vb" + std::to_string(j), {s.location, s.words});
    }
    for (const auto &[i, p] : r.programs)
        fprintf(out, "P prog %d %s %x %s\n", i, p.id.c_str(), p.offset, p.known ? "known" : "unknown");
    fprintf(out, "P draw %u %u %x\n", r.first, r.count, r.begin);
    fprintf(out, "P end\n");
}

}  // namespace

void Decoder::probeDump(uint32_t first, uint32_t count)
{
    ProbeRecord r;
    probeRecord(r);
    r.first = first;
    r.count = count;
    printProbe(out_, r);
    // NVDEC_SCRUB: zero set 0 and what its pointers reach once read, so that the next draw's descriptors and payloads show only
    // the bytes m2v writes for it (its allocations are recycled: what it does not write would otherwise be the last draw's)
    if (scrub && getenv("NVDEC_SCRUB")) {
        if (const std::vector<uint32_t> &d = r.sets[0].words; !d.empty()) {
            for (size_t i = 0; i + 1 < d.size(); i += 2) {
                const uint64_t v = (uint64_t(d[i + 1]) << 32) | d[i];
                if ((v >> 32) >= 1 && (v >> 32) < 0x80 && map_ && map_(v, 1))
                    scrub(v, 256);
            }
            scrub(r.sets[0].va, d.size() * 4);
        }
        for (const auto &[j, s] : r.streams)
            scrub(s.location, 256);
    }
}

const ProgramInfo *Decoder::programById(const std::string &id) const
{
    for (const auto &kv : programs)
        if (kv.second.id == id)
            return &kv.second;
    return nullptr;
}

bool Decoder::bound(int group, int slot, uint64_t &addr, uint32_t &bytes) const
{
    if (group < 0 || group > 4 || slot < 0 || slot >= 18 || !binds_[group][slot].valid)
        return false;
    addr = binds_[group][slot].addr;
    bytes = binds_[group][slot].size;
    return true;
}

void Decoder::pushWords(uint32_t &mask, uint32_t words[32]) const
{
    mask = 0;
    const uint64_t root = binds_[4][0].valid ? binds_[4][0].addr : binds_[0][0].addr;
    for (auto it = writtenThisSubmit_.lower_bound(root + 0x700); it != writtenThisSubmit_.end() && *it < root + 0x780;
         ++it) {
        const uint32_t k = uint32_t(*it - root - 0x700) / 4;
        mask |= 1u << k;
        words[k] = cbufWrites_.at(*it);
    }
}

// Whether a live stage's program reads a constant buffer (not the root table, which LOAD_CONSTANT_BUFFER writes in order) over
// bytes in constantStale_
bool Decoder::readsStaleConstants()
{
    for (int i = 0; i < 6; i++) {
        const uint32_t group = r3(PIPELINE + 0x40 * i + 0x10);
        if (!(r3(PIPELINE + 0x40 * i) & 1) || group >= 5)
            continue;
        std::string id;
        const ProgramInfo *info = program(i, id);
        const size_t slots = info ? info->cbufTypes.size() : 18;
        for (size_t s = 0; s < slots; s++) {
            const Bind &b = binds_[group][s];
            const uint8_t type = info ? info->cbufTypes[s] & 7 : 0;
            if (!b.valid || type == 1 || (type == 0 && s == 0))
                continue;
            for (const auto &r : constantStale_)
                if (b.addr < r.second && r.first < b.addr + b.size)
                    return true;
        }
    }
    return false;
}

}  // namespace nvdec
