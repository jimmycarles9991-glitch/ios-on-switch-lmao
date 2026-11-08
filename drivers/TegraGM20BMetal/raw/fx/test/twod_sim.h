// SPDX-License-Identifier: MIT
// raw/fx host test (r12-cafx): a model of the GM20B front end and FERMI_TWOD_A for push words fx emits. It decodes the method
// stream (NVIDIA host FIFO headers: INC, NON_INC, IMMD, ONE_INC), keeps the 2D engine's registers, and on each launch
// (PIXELS_FROM_MEMORY_SRC_Y0_INT) copies or filters texels between surfaces in host memory: pitch or block linear (TegraColor
// GOBs, the block height the surface was set up with, one GOB a tile in x). Sampling follows the class's documented semantics
// (SAMPLE_MODE origin centre: source position of destination texel x = SRC_X0 + (x - DST_X0 + 0.5) * DU_DX; bilinear between the
// two nearest texel centres, clamped to the edge, 1/256 weights).
//
// It also checks the ordering the hardware needs, and fails the run if a launch reads a surface byte range a previous launch
// wrote without a WAIT_FOR_IDLE and then a TWOD_INVALIDATE_TEXTURE_DATA_CACHE in between (the 2D engine reads its source through
// the texture data cache), and records whether the stream ends with a 3D WAIT_FOR_IDLE + INVALIDATE_TEXTURE_DATA_CACHE after
// the last 2D write (draws sample the result).
//
// What it cannot prove: the hardware's arithmetic and its exact sample position for scaled blits (fx_mip.h: the device
// self-test does), and that the texture unit reads the chain where NIL says (the NIL oracle compares the layout with Mesa's own).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace twodsim {

struct Region {
    uint64_t va;
    std::vector<uint8_t> *bytes;
};

class Sim {
public:
    void map(uint64_t va, std::vector<uint8_t> &bytes)
    {
        regions_.push_back({va, &bytes});
    }

    // memory outside the mapped regions (r8-rawca: the host tests' mock GPU, pack/nvdec.cpp with NVDEC_TWOD: every BO it maps)
    std::function<uint8_t *(uint64_t va, uint64_t bytes)> host;

    // one method of a submission (the mock GPU's decoder feeds the 2D subchannel's, and the 3D wait and texture invalidate);
    // false once a launch failed (error())
    bool feed(uint32_t subc, uint32_t mthd, uint32_t data)
    {
        if (subc == 3 || (subc == 0 && (mthd == 0x0110 || mthd == 0x1288)))
            method(subc, mthd, data);
        return !failed_;
    }

    // runs `n` push words; false (and error()) on an unknown header, an unmapped access, an unsupported format or a hazard
    bool run(const uint32_t *w, size_t n);

    const std::string &error() const
    {
        return err_;
    }

    uint32_t launches = 0, waits2d = 0, waits3d = 0, texInvalidates2d = 0, texInvalidates3d = 0;
    bool endsSampleable =
        false;  // the last 2D launch is followed by a 3D WAIT_FOR_IDLE and a 3D texture data cache invalidate
    // GM20B as measured: its centre-origin scaled blit samples at SRC_X0 + (x - DST_X0) * DU_DX + 1/2, half a source texel
    // where the class documents half a step; the corner origin as documented (fx::kGm20bSampling's comment: the device's sweep)
    bool quirkCentreHalfTexel = false;

private:
    struct Range {
        uint64_t begin, end;
        bool waited;  // a WAIT_FOR_IDLE came after the write
    };

    std::vector<Region> regions_;
    uint32_t r2d_[0x1000] = {};
    std::vector<Range> pending_;  // written by launches, not yet invalidated in the texture data cache
    std::string err_;
    bool sawWait3dAfter2d_ = false, sawInv3dAfter2d_ = false;
    uint8_t *at(uint64_t va, uint64_t bytes);
    void method(uint32_t subc, uint32_t mthd, uint32_t data);
    bool launch();

    struct Surface {
        uint32_t format, bpp, layout, yLog2, pitch, width, height;
        uint64_t va;
    };

    Surface surface(uint32_t base) const;
    uint64_t texel(const Surface &s, uint32_t x, uint32_t y) const;  // GPU address of texel (x, y)
    bool failed_ = false;
};

}  // namespace twodsim
