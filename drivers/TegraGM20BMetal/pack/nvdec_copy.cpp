// SPDX-License-Identifier: MIT
#include "nvdec.h"
#include "TegraGM20BLayout.h"

#include <algorithm>
#include <cstring>

namespace nvdec {

void Decoder::landCopies()
{
    for (const CopyStore &s : copyStores_)
        for (size_t k = 0; k < s.bytes.size();) {
            size_t end = k + 1;
            if (s.written.empty())
                end = s.bytes.size();
            else
                while (end < s.bytes.size() && s.written[end] == s.written[k])
                    end++;
            if (s.written.empty() || s.written[k])
                storeCopy(s.at + k, &s.bytes[k], end - k);
            k = end;
        }
    copyStores_.clear();
}

// NVDEC_WRITETHROUGH: the copy lands in the memory behind the destination too, as the GPU's would, so a host test reads its
// result back on the CPU (pack/cap.cpp maps every BO) (and not in the overlay: the CPU may rewrite that memory later, which the
// overlay would hide from the next read)
void Decoder::storeCopy(uint64_t at, const uint8_t *bytes, uint64_t n)
{
    static const bool through = getenv("NVDEC_WRITETHROUGH") != nullptr;
    if (through && map_)
        if (const uint8_t *h = map_(at, n)) {
            memcpy(const_cast<uint8_t *>(h), bytes, n);
            return;
        }
    for (uint64_t k = 0; k < n; k += 4) {
        uint32_t w = 0;
        if (k + 4 <= n && ((at + k) & 3) == 0)
            memcpy(&w, &bytes[k], 4);
        else {  // an unaligned edge: merge into the word
            const uint64_t a = (at + k) & ~3ull;
            read(a, &w, 4);
            for (uint64_t b = 0; b < 4 && k + b < n; b++)
                reinterpret_cast<uint8_t *>(&w)[((at + k) & 3) + b < 4 ? ((at + k) & 3) + b : 3] = bytes[k + b];
            cbufWrites_[a] = w;
            continue;
        }
        cbufWrites_[at + k] = w;
    }
}

// MAXWELL_DMA_COPY_A LAUNCH_DMA: a copy (NVK's upload queue puts shaders and data in place with these; the driver's
// copies) or, with REMAP to constant A, a fill. Either side may be block-linear (SET_SRC/DST_BLOCK_SIZE: one GOB wide, its
// block height, the surface's width in elements and height, the copy's origin), addressed as TegraGM20BLayout.h lays the
// GPU's tiled memory out; NVK's optimal images and the driver's block-linear textures are. The GPU's writes land in the
// same overlay as LOAD_CONSTANT_BUFFER's.
void Decoder::copyEngine(uint32_t launch)
{
    const uint32_t *r = reg_[4];
    const uint64_t src = (uint64_t(r[0x400 / 4] & 0xff) << 32) | r[0x404 / 4],
                   dst = (uint64_t(r[0x408 / 4] & 0xff) << 32) | r[0x40c / 4];
    const uint32_t pitchIn = r[0x410 / 4], pitchOut = r[0x414 / 4], length = r[0x418 / 4];
    const uint32_t lines = (launch & (1u << 9)) ? r[0x41c / 4] : 1;
    const bool srcPitch = launch & (1u << 7), dstPitch = launch & (1u << 8);
    // remap: each destination component of COMPONENT_SIZE bytes from a source component (SRC_X..W of the source element of
    // NUM_SRC_COMPONENTS), constant A or B, or left as it was (NO_WRITE); the line length counts elements (nouveau_copy_rect).
    // NVK's two uses: DST_X CONST_A, the fill (vkCmdFillBuffer), and DST_X..W = SRC_X..W, an image copy; the driver's planar
    // conversion (r8-rawca: a 4:4:4 two-plane surface's planes into one RGBA8 image) leaves components with NO_WRITE.
    uint32_t elem = 1, srcElem = 1, comp = 0, csize = 1, nsrc = 1, ndst = 1;
    const uint32_t consts[2] = {r[0x700 / 4], r[0x704 / 4]};
    bool remap = false, plain = true;
    if (launch & (1u << 10)) {
        remap = true;
        comp = r[0x708 / 4];
        csize = ((comp >> 16) & 3) + 1;
        nsrc = ((comp >> 20) & 3) + 1;
        ndst = ((comp >> 24) & 3) + 1;
        elem = csize * ndst;
        srcElem = csize * nsrc;
        for (uint32_t c = 0; c < ndst; c++)
            plain = plain && ((comp >> (4 * c)) & 7) == c && c < nsrc;
        plain = plain && nsrc == ndst;
    }
    bool reads = !remap || plain;
    for (uint32_t c = 0; remap && c < ndst; c++)
        reads = reads || ((comp >> (4 * c)) & 7) < 4;

    // where element e of line y lies: rows `pitch` apart from `base`, or a block-linear surface (its six block words at `at`:
    // block height and depth, width in elements, height, depth and the layer the copy reads or writes)
    struct Side {
        bool tiled;
        uint64_t base;
        uint32_t pitch, yLog2, rowBytes, ox, oy, zLog2 = 0, height = 0, layer = 0;
    };

    auto side = [&](bool isPitch, uint64_t base, uint32_t pitch, uint32_t at, uint32_t elemBytes) {
        Side d{!isPitch, base, pitch, 0, 0, 0, 0};
        if (d.tiled) {
            d.yLog2 = (r[at / 4] >> 4) & 0xf;
            d.zLog2 = (r[at / 4] >> 8) & 0xf;
            d.rowBytes = r[at / 4 + 1] * elemBytes;
            d.height = r[at / 4 + 2];
            d.layer = r[at / 4 + 4];
            d.ox = r[at / 4 + 5] & 0xffff;
            d.oy = r[at / 4 + 5] >> 16;
        }
        return d;
    };
    const Side in = side(srcPitch, src, pitchIn, 0x728, srcElem), out = side(dstPitch, dst, pitchOut, 0x70c, elem);
    // a constant buffer is pitch memory: block-linear destinations are images
    if (dstPitch && length && lines) {
        constantStale_.push_back({dst, dst + uint64_t(lines - 1) * pitchOut + uint64_t(length) * elem});
        // one hull of them all: may see a stale read where there is none, never miss one
        if (constantStale_.size() > 1024) {
            uint64_t lo = ~0ull, hi = 0;
            for (const auto &r : constantStale_) {
                lo = std::min(lo, r.first);
                hi = std::max(hi, r.second);
            }
            constantStale_.assign(1, {lo, hi});
        }
    }
    auto addr = [](const Side &d, uint32_t e, uint32_t y, uint32_t elemBytes) {
        if (!d.tiled)
            return d.base + uint64_t(y) * d.pitch + uint64_t(e) * elemBytes;
        if (!d.zLog2 && !d.layer)
            return d.base + tgm_layout_offset_of(d.rowBytes, uint8_t(d.yLog2), (d.ox + e) * elemBytes, d.oy + y);
        // a plane of a 3D surface: blocks along a row, down the plane, then through the depth; GOBs down a block, then
        // through its planes
        const uint32_t xB = (d.ox + e) * elemBytes, row = d.oy + y, blockRows = 8u << d.yLog2,
                       blockPlanes = 1u << d.zLog2;
        const uint64_t blocksDown = (d.height + blockRows - 1) / blockRows;
        const uint64_t block =
            (uint64_t(d.layer / blockPlanes) * blocksDown + row / blockRows) * (d.rowBytes / 64) + xB / 64;
        const uint64_t gob = uint64_t(d.layer % blockPlanes) * (blockRows / 8) + (row % blockRows) / 8;
        return d.base + block * (512ull << (d.yLog2 + d.zLog2)) + gob * 512 + tgm_layout_gob_offset(xB, row);
    };
    if ((launch & 3) != 1)  // not PIPELINED
        landCopies();
    // NO_WRITE and missing source components leave the destination's bytes as they are when the copy lands
    std::vector<uint8_t> written;
    for (uint32_t c = 0; remap && !plain && c < ndst; c++) {
        const uint32_t sel = (comp >> (4 * c)) & 7;
        written.insert(written.end(), csize, (sel < 4 && sel < nsrc) || sel == 4 || sel == 5);
    }
    const size_t first = copyStores_.size();
    auto store = [&](uint64_t at, const uint8_t *bytes, uint64_t n) {
        if (copyStores_.size() == first || copyStores_.back().at + copyStores_.back().bytes.size() != at)
            copyStores_.push_back({at, {}, {}});
        CopyStore &s = copyStores_.back();
        s.bytes.insert(s.bytes.end(), bytes, bytes + n);
        for (uint64_t k = 0; !written.empty() && k < n; k += elem)
            s.written.insert(s.written.end(), written.begin(), written.end());
    };
    const uint64_t bytes = uint64_t(length) * elem;
    std::vector<uint8_t> line(bytes), from(uint64_t(length) * srcElem);
    for (uint32_t y = 0; y < lines; y++) {
        if (reads) {
            if (in.tiled)
                for (uint32_t e = 0; e < length; e++)
                    read(addr(in, e, y, srcElem), &from[uint64_t(e) * srcElem], srcElem);
            else
                read(addr(in, 0, y, srcElem), from.data(), from.size());
        }
        if (remap && !plain) {
            for (uint32_t e = 0; e < length; e++)
                for (uint32_t c = 0; c < ndst; c++) {
                    const uint32_t sel = (comp >> (4 * c)) & 7;
                    uint8_t *o = &line[uint64_t(e) * elem + c * csize];
                    if (sel < 4 && sel < nsrc)
                        memcpy(o, &from[uint64_t(e) * srcElem + sel * csize], csize);
                    else if (sel == 4 || sel == 5)
                        for (uint32_t k = 0; k < csize; k++)
                            o[k] = uint8_t(consts[sel - 4] >> (8 * k));
                }
        } else {
            line.assign(from.begin(), from.begin() + bytes);
        }
        if (out.tiled)
            for (uint32_t e = 0; e < length; e++)
                store(addr(out, e, y, elem), &line[uint64_t(e) * elem], elem);
        else
            store(addr(out, 0, y, elem), line.data(), bytes);
    }
}

bool Decoder::read(uint64_t va, void *dst, uint64_t bytes)
{
    const uint8_t *src = map_ ? map_(va, bytes) : nullptr;
    if (src)
        memcpy(dst, src, bytes);
    else if (map_ && bytes) {  // across binds (NVK grows its shader heap by binding more BOs after it): page by page
        bool any = false;
        for (uint64_t at = 0; at < bytes;) {
            const uint64_t n = std::min<uint64_t>(bytes - at, 0x1000 - ((va + at) & 0xfff));
            const uint8_t *s = map_(va + at, n);
            if (s) {
                memcpy(static_cast<uint8_t *>(dst) + at, s, n);
                any = true;
            } else
                memset(static_cast<uint8_t *>(dst) + at, 0, n);
            at += n;
        }
        src = any ? static_cast<const uint8_t *>(dst) : nullptr;
    } else
        memset(dst, 0, bytes);
    // the GPU's writes over it: every overlay word the range overlaps, its overlapping bytes (a row of a copy whose length is no
    // multiple of 4 ends in part of a word: r18, NVDEC_TEXELS over rows of 1025 bytes)
    for (auto it = cbufWrites_.lower_bound(va >= 3 ? va - 3 : 0); it != cbufWrites_.end() && it->first < va + bytes;
         ++it) {
        const uint64_t lo = std::max(it->first, va), hi = std::min(it->first + 4, va + bytes);
        if (lo < hi)
            memcpy(static_cast<uint8_t *>(dst) + (lo - va),
                   reinterpret_cast<const uint8_t *>(&it->second) + (lo - it->first), hi - lo);
    }
    return src != nullptr;
}

}  // namespace nvdec
