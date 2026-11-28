// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "barrier_forms.h"
#include "fresh.h"
#include "loop_copies.h"
#include "probe_ops.h"
#include "push.h"
#include "resources.h"

namespace gm20b {

class CommandBuffer;
class Device;
struct CopySurface;

// Copy-engine work between passes (MAXWELL_DMA_COPY_A, as NVK's nouveau_copy_rect does it for pitch memory)
class BlitEncoder {
public:
    // VA-only operands borrow backing owners already named by CommandBuffer::use/useAddress.
    // `bytes` bytes from `src` to `dst` (vkCmdCopyBuffer)
    void copyBytes(uint64_t src, uint64_t dst, uint64_t bytes);
    // `bytes` bytes at `dst` set to `value` (fillBuffer:range:value:): the copy engine writing a constant, its whole words as
    // NVK's vkCmdFillBuffer (nvk_cmd_fill_memory_ce), the bytes before and after them one at a time
    void fillBytes(uint64_t dst, uint64_t bytes, uint8_t value);
    // `bytes` (whole words) at `dst` set to the 32-bit `word`
    void fillWords(uint64_t dst, uint64_t bytes, uint32_t word);
    // `ms`'s sample grid (2^sxLog2 x 2^syLog2 samples a pixel) halved into [0, width) x [0, height) of `into` by the 2D
    // engine (resolve_twod.cpp); `complement`: through the grid's complement (the probe's, resolve_rule.h), the
    // grid complemented back after where `keep`
    void resolveTwod(const Texture &ms, uint32_t sxLog2, uint32_t syLog2, const Texture &into, uint32_t width,
                     uint32_t height, uint32_t fmt2d, bool complement, bool keep);
    // One-sample image `one` into every sample of `ms`'s sample grid over [0, width) x [0, height) of its pixels, each
    // sample its pixel's texel, by the 2D engine's point-sampled scale (resolve_twod.cpp)
    void spreadTwod(const Texture &one, const Texture &ms, uint32_t sxLog2, uint32_t syLog2, uint32_t width,
                    uint32_t height, uint32_t fmt2d);
    // [x0, x1) x [y0, y1) of `t` set to `word` by the 2D engine's solid rectangle (rule clear.twod, twod_fill.h)
    void fillTwod(const Texture &t, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1, uint32_t word);
    // `lines` rows of `width` pixels of `bpp` bytes, from `src` (row stride srcPitch) to `dst` (dstPitch) (vkCmdCopyImage and the
    // buffer<->image copies of linear images)
    void copyRect(uint64_t src, uint32_t srcPitch, uint64_t dst, uint32_t dstPitch, uint32_t width, uint32_t lines,
                  uint32_t bpp);
    // `w` x `h` texels from (sx, sy) of plane sz of `src` to (dx, dy) of plane dz of `dst`, each pitch or block-linear (a 2D
    // texture's plane is 0) (vkCmdCopyImage). False only when an overlapping copy cannot allocate staging memory.
    bool copyTexels(const Texture &src, uint32_t sx, uint32_t sy, uint32_t sz, const Texture &dst, uint32_t dx,
                    uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint8_t aspect = kAspectWhole);
    // `w` x `h` texels from the origin of plane z of `t` (a chain level: chainLevelImage) each set to `fill`'s texel
    void fillTexels(const Texture &t, uint32_t z, uint32_t w, uint32_t h, const CopyFill &fill);
    // `w` x `h` units at (x, y) of plane or layer z of `t` each set to `texel`'s `bytes` (1, 2, 4, 8 or 16), only its 4-byte
    // words `words` names (bit k: word k; 0: every byte) where the rest is kept (a fill of Depth32Float_Stencil8's depth or
    // its stencil view's stencil): the copy engine's constants, two values a launch (fillTexture:, fill_record.h)
    void fillRect(const Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w, uint32_t h, const uint8_t texel[16],
                  uint32_t bytes, uint32_t words);
    // `w` x `h` stencil bytes between (x, y) of plane z of Depth32Float_Stencil8 `zeta` (its stencilTemp made) and (bx, by)
    // of plane bz of `bytes`, a texture of one-byte texels: into the zeta where `intoZeta`, else out of it. Through the
    // temp, as copyBufferTexels moves a buffer's (NVK's stencil_copy_temp), a part of the temp's size at a time
    void copyStencilTexels(const Texture &zeta, uint32_t x, uint32_t y, uint32_t z, const Texture &bytes, uint32_t bx,
                           uint32_t by, uint32_t bz, uint32_t w, uint32_t h, bool intoZeta);
    // The first byte of each texel of `from` into the first byte of `to`'s, every other byte of `to` as it was: one remapped
    // copy of one-byte components over their common extent (an R8Unorm_sRGB target's stand-in, format_standin.h)
    void copyFirstByte(const Texture &from, const Texture &to);
    // The same of the first two bytes (an RG8Unorm_sRGB target's stand-in)
    void copyFirstTwoBytes(const Texture &from, const Texture &to);
    // An ARGB8Unorm texture's texels into its RGBA8 stand-in (`toStandIn`) or back, each texel's bytes rotated by one by
    // the copy engine's component remap (format_standin.h)
    void copyArgb8(const Texture &from, const Texture &to, bool toStandIn);
    // `w` x `h` texels of one RGBA8 or BGRA8 texture into one of the other order, each code kept (a one-sample
    // attachment's resolve across the orders, as an M4 Max makes it: TegraGM20BMetalRangeSeal.mm resolvePasses)
    void copyRedBlueSwapped(const Texture &from, const Texture &to, uint32_t w, uint32_t h);
    // `w` x `h` texels between rows `pitch` bytes apart at `va` (both multiples of the texel's component, at most 4 bytes) and
    // (x, y) of plane z of `t`, pitch or block-linear: into `t` where `toTexture`, else out of it (vkCmdCopyBufferToImage,
    // vkCmdCopyImageToBuffer); the buffer's texels bufferUnit each. A Depth32Float_Stencil8 aspect's copy is NVK's
    // (nvk_remap_extract_aspect, insert_aspect): depth its texel's first word, stencil its fifth byte through t.stencilTemp.
    // A multisampled `t`'s buffer texels are each texel's samples in order (copyBufferSamples)
    void copyBufferTexels(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t z, uint32_t w,
                          uint32_t h, bool toTexture, uint8_t aspect = kAspectWhole);
    // A planar texture's image from its surface's planes (Texture::planar): two remapped copies, the luma codes into green
    // (alpha 255), then the chroma pairs into red (Cr) and blue (Cb), each leaving the other components as they are
    void convertPlanar(const Texture &t);
    // An RG8Unorm_sRGB texture's stand-in from its bytes (srgb_pair.h): a remapped copy a 2D image
    // (xr_parts.h); false: not one with a stand-in of its texels
    bool widenSrgbPair(const Texture &t);
    // A one-plane 4:2:2 YCbCr texture's luma and chroma images from its plane (Texture::onePlane): two remapped copies of
    // its Y0 Cb Y1 Cr texel pairs, Y0 and Y1 into the luma image, Cb and Cr into the chroma image
    void splitPlane(const Texture &t);
    // Its inverse after a pass stored into such a texture (ycbcr_store.h): Y0 and Y1 of the luma image and Cb and Cr of the
    // chroma image into their bytes of each pair, the other two NO_WRITE; an odd width's last pair keeps its second luma byte
    void mergePlane(const Texture &t);
    // generateMipmaps of a mipmapped texture (Texture::chain) on the 2D engine as surface format `fmt2d`: levels first + 1 ..
    // last from the one above, in place (fx_mip_route.h MipRun; `unproven` of its steps); `point`: an integer format's
    // levels, each texel one of the level above's (fx::emitPointGenerate)
    void generate(Texture &t, uint32_t fmt2d, uint32_t first, uint32_t last, uint32_t unproven, uint32_t firstLayer,
                  uint32_t layers, bool point = false);
    // The next command reads the memory `read` names and writes the memory `write` names (null: none; a name is any pointer
    // that stands for one memory): where it reads or writes what an earlier command of this encoder wrote, or writes what one
    // read, it waits for those first (barrier), as Metal orders a blit encoder's commands on one resource
    void access(const void *read, const void *write);
    // Every copy and 2D blit before it completes before any after it starts: NVK's wait between transfers
    // (nvk_cmd_flush_wait_dep), as the copy engine starts a PIPELINED launch before the ones before it have landed
    void barrier();
    // Between this encoder's accesses of the same memory through two PTE kinds (a pool image's texels and its kind-0
    // view, Device::blPitchVa): everything before completes and the L2 drops its lines (switch_gm20b_kind_switch)
    void crossKinds();

    bool emitted() const
    {
        return p_.at != opened_;
    }

    // A counter sample on the copy engine now; `afterCopies`: once the copies before it landed
    void timestamp(uint64_t va, bool afterCopies);
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    // hw_test_app's 2D-engine probes (probe_ops.h): `op` reading `read` and writing `written`
    void probe(const ProbeOp &op, const Texture *read, const Texture &written);
#endif
    void end();

private:
    friend class CommandBuffer;
    CommandBuffer *cb_ = nullptr;
    Device *dev_ = nullptr;
    Push p_;
    uint32_t *start_ = nullptr;
    // the engine of the work before it and where its opening ended: an encoder that sent nothing after its opening
    // changed no engine, and leaves the command buffer's as it found it
    BarrierEngine before_ = BarrierEngine::None;
    bool beforePassShaderFlushed_ = false, beforeComputeReleased_ = false;
    uint32_t *opened_ = nullptr;
    static constexpr uint8_t kAccesses = 16;
    const void *read_[kAccesses] = {}, *written_[kAccesses] = {};
    uint8_t reads_ = 0, writes_ = 0;
    void room(uint32_t words = 56);  // at least `words` words left in the reservation
    // `w` x `h` elements through `remap` (copyRemapped), in launches whose lines each fit a row of every pitch side: the
    // GM20B's copy engine never completes a launch of more than one line longer than a pitch side's rows, and the
    // channel's context switch then times out with the engine busy. Such a copy goes a line at a time, or in columns no
    // wider than those rows, whichever takes fewer launches; where the destination's rows overlap, each launch after the
    // one before, a later row's bytes landing last
    void copyElements(const CopySurface &s, const CopySurface &d, uint32_t w, uint32_t h, uint32_t remap,
                      uint32_t bytes, bool ordered = false);
    // copyBufferTexels of a multisampled colour texture, an M4 Max's layout both ways: row y of the buffer holds texel
    // (x, y)'s samples in order. A 2x sample grid's rows are those rows; a 4x grid's rows 2y and 2y + 1 meet in buffer
    // row y two samples at a time, through t.sampleRows (made at record: bufferCopy): the grid's rows moved whole
    // between the texture and the temp, the temp's even and odd rows between alternate pairs of each buffer texel by
    // remapped launches (texels of up to four bytes), else a launch a column or a row of pairs, whichever are fewer
    void copyBufferSamples(uint64_t va, uint32_t pitch, const Texture &t, uint32_t x, uint32_t y, uint32_t w,
                           uint32_t h, bool toTexture);
    void fillRun(uint64_t at, uint32_t width, uint32_t height, uint32_t unit);  // a constant fill's launch
    LoopStream stream();
};

}  // namespace gm20b
