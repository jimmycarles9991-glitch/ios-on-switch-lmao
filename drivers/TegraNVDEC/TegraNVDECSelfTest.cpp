// SPDX-License-Identifier: BSD-2-Clause-Patent
// N3 (stage 2 on): NVDEC decodes one embedded H.264 picture through the same path user clients use
// (tegra_nvdec_run: checker, relocations, kernel setup slot, syncpoint fence by interrupt). The picture is a 64x64
// High-profile IDR (CABAC, 8x8 transform); its picture setup, bitstream and slice offsets were produced on the host by
// the plug-in's own H.264 code (SwitchVideoDecoders/test/h264_host --selftest), and the expected luma CRC is x264's
// reconstruction. Passing proves firmware, method stream, setup layout and the block-linear surface layout together.
// VP9 (libvpx's key frame) and HEVC (x265's IDR) each have a picture of their own below.
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <string.h>
#include "TegraNVDEC.h"
#include "nvidia/nvdec_drv.h"
#include "nvidia/clc5b0.h"
#include "selftest/H264Picture64.h"
#include "selftest/VP9Picture160.h"
#include "selftest/HEVCPicture176.h"

namespace {
enum {
    kInput = 1,
    kAux = 2,
    kSurface = 3
};

constexpr uint32_t kSlicesOff = 256, kBitsOff = 512, kMbhistOff = 0x8000, kHistoryOff = 0x9000;
TegraNVDECBlock gBlocks[4];

bool resolve(void *, uint32_t handle, TegraNVDECBlock *block)
{
    if (handle < kInput || handle > kSurface)
        return false;
    *block = gBlocks[handle];
    return true;
}

// Byte offset of (x, y) in a block-linear plane with 64Bx8 GOBs stacked two to a block (GOB height 2).
uint32_t blockLinear(uint32_t x, uint32_t y, uint32_t pitch)
{
    const uint32_t gob = ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + x % 16;
    return (y / 16) * pitch * 16 + (x / 64) * 1024 + ((y % 16) / 8) * 512 + gob;
}

uint32_t crc32(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int i = 0; i < 8; i++)
        crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    return crc;
}

void entry(TegraNVDECSubmit *s, uint16_t method, uint16_t kind, uint32_t value, uint32_t offset)
{
    s->entries[s->entry_count++] = {method, kind, value, offset};
}

uint64_t nowUs()
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

int run(char *line, size_t size)
{
    const nvdec_h264_pic_s *setup = (const nvdec_h264_pic_s *)kTestSetup;
    const uint32_t width = kTestWidthMbs * 16, height = kTestHeightMbs * 16, pitch = setup->pitch_luma;
    const uint32_t lumaBytes = pitch * ((height + 31) & ~31u);
    for (int i = kInput; i <= kSurface; i++) {
        if (tegra_nvdec_buffer_alloc(64 * 1024, &gBlocks[i])) {
            snprintf(line, size, "N3 FAIL no NVDEC memory");
            return -12;
        }
    }
    memcpy(gBlocks[kInput].va + kSlicesOff, kTestSliceOffsets, sizeof(kTestSliceOffsets));
    memcpy(gBlocks[kInput].va + kBitsOff, kTestBitstream, sizeof(kTestBitstream));

    TegraNVDECSubmit *s = IONewZero(TegraNVDECSubmit, 1);
    if (!s)
        return -12;
    s->codec = TEGRA_NVDEC_CODEC_H264;
    s->setup_bytes = sizeof(kTestSetup);
    memcpy(s->setup, kTestSetup, sizeof(kTestSetup));
    entry(s, NVC5B0_SET_APPLICATION_ID, TEGRA_NVDEC_ENTRY_VALUE, NVC5B0_SET_APPLICATION_ID_ID_H264, 0);
    entry(s, NVC5B0_SET_CONTROL_PARAMS, TEGRA_NVDEC_ENTRY_VALUE,
          NVC5B0_SET_CONTROL_PARAMS_CODEC_TYPE_H264 | 1u << 4 | 1u << 6, 0);
    entry(s, NVC5B0_SET_PICTURE_INDEX, TEGRA_NVDEC_ENTRY_VALUE, 0, 0);
    entry(s, NVC5B0_SET_IN_BUF_BASE_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, kBitsOff);
    entry(s, NVC5B0_SET_SLICE_OFFSETS_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, kSlicesOff);
    entry(s, NVC5B0_SET_NVDEC_STATUS_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kInput, 0);
    entry(s, NVC5B0_SET_COLOC_DATA_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, 0);
    entry(s, NVC5B0_H264_SET_MBHIST_BUF_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, kMbhistOff);
    entry(s, NVC5B0_SET_HISTORY_OFFSET, TEGRA_NVDEC_ENTRY_RELOC, kAux, kHistoryOff);
    for (uint16_t i = 0; i < 17; i++) {
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_LUMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, kSurface, 0);
        entry(s, (uint16_t)(NVC5B0_SET_PICTURE_CHROMA_OFFSET0 + 4 * i), TEGRA_NVDEC_ENTRY_RELOC, kSurface, lumaBytes);
    }
    entry(s, NVC5B0_EXECUTE, TEGRA_NVDEC_ENTRY_VALUE, 1u << 8, 0);
    s->clean[0] = {kInput, kSlicesOff, (uint32_t)sizeof(kTestSliceOffsets), 0};
    s->clean[1] = {kInput, kBitsOff, (uint32_t)sizeof(kTestBitstream), 0};
    s->clean_count = 2;

    const uint64_t began = nowUs();
    uint32_t fence = 0;
    const char *why = nullptr;
    int ret = tegra_nvdec_run(s, resolve, nullptr, &fence, &why);
    IODelete(s, TegraNVDECSubmit, 1);
    if (ret) {
        snprintf(line, size, "N3 FAIL submit refused (%d, %s)", ret, why ? why : "engine");
        return ret;
    }
    if ((ret = tegra_nvdec_wait(fence))) {
        snprintf(line, size, "N3 FAIL decode did not finish (%s; see the job line)",
                 ret == -60  ? "past its bound"
                 : ret == -5 ? "done, but its interrupt never came"
                             : "engine refused");
        return ret;
    }
    const uint64_t us = nowUs() - began;
    tegra_nvdec_cache_flush(gBlocks[kInput].va, sizeof(nvdec_status_s));
    tegra_nvdec_cache_flush(gBlocks[kSurface].va, lumaBytes * 3 / 2);
    const nvdec_status_s *status = (const nvdec_status_s *)gBlocks[kInput].va;
    uint32_t crc = 0xffffffffu;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            crc = crc32(crc, gBlocks[kSurface].va[blockLinear(x, y, pitch)]);
    crc = ~crc;
    const bool ok = crc == kTestLumaCrc && !status->error_status && !status->mbs_in_error;
    snprintf(line, size,
             "N3 %s H.264 %ux%u IDR in %llu us: %u MBs decoded, %u in error, error_status %#x, %u cycles; "
             "luma CRC %#010x (expected %#010x)",
             ok ? "ok" : "FAIL", width, height, us, status->mbs_correctly_decoded, status->mbs_in_error,
             status->error_status, status->cycle_count, crc, kTestLumaCrc);
    return ok ? 0 : -5;
}

// N3 VP9: libvpx's 160x144 key frame through the plug-in's own method stream and buffer layout (VP9Picture160.h, made
// by SwitchVideoDecoders/test/vp9_host --selftest), luma CRC against libvpx's decode. Buffers 1 input (status,
// probability table, tile data), 2 aux (tile sizes, segmentation maps, counters, motion vectors, filter), 3 surface.
TegraNVDECBlock gVP9Blocks[4];

bool resolveVP9(void *, uint32_t handle, TegraNVDECBlock *block)
{
    if (handle < 1 || handle > 3)
        return false;
    *block = gVP9Blocks[handle];
    return true;
}

int runVP9(char *line, size_t size)
{
    for (int i = 1; i <= 3; i++) {
        if (tegra_nvdec_buffer_alloc(kVP9TestBytes[i], &gVP9Blocks[i])) {
            snprintf(line, size, "N3 VP9 FAIL no NVDEC memory");
            return -12;
        }
    }
    uint8_t *input = gVP9Blocks[1].va, *aux = gVP9Blocks[2].va;
    memcpy(input + kVP9TestProbOff, kVP9TestProbs, sizeof(kVP9TestProbs));
    memcpy(input + kVP9TestBitsOff, kVP9TestBitstream, sizeof(kVP9TestBitstream));
    memset(input + kVP9TestBitsOff + sizeof(kVP9TestBitstream), 0, 64);  // the plug-in's padding after the tile data
    memcpy(aux + kVP9TestTilesOff, kVP9TestTiles, sizeof(kVP9TestTiles));
    ((uint16_t *)(aux + kVP9TestTilesOff))[0x37a] = 9;  // the tile size table's tail, as the plug-in sets it
    ((uint16_t *)(aux + kVP9TestTilesOff))[0x37b] = 1;
    TegraNVDECSubmit *s = IONewZero(TegraNVDECSubmit, 1);
    if (!s)
        return -12;
    s->codec = TEGRA_NVDEC_CODEC_VP9;
    s->setup_bytes = sizeof(kVP9TestSetup);
    memcpy(s->setup, kVP9TestSetup, sizeof(kVP9TestSetup));
    s->entry_count = sizeof(kVP9TestEntries) / sizeof(kVP9TestEntries[0]);
    memcpy(s->entries, kVP9TestEntries, sizeof(kVP9TestEntries));
    s->clean[0] = {1, kVP9TestProbOff, (uint32_t)(kVP9TestBitsOff - kVP9TestProbOff + sizeof(kVP9TestBitstream) + 64),
                   0};
    s->clean[1] = {2, kVP9TestTilesOff, 0x700, 0};
    s->clean_count = 2;

    const uint64_t began = nowUs();
    uint32_t fence = 0;
    const char *why = nullptr;
    int ret = tegra_nvdec_run(s, resolveVP9, nullptr, &fence, &why);
    IODelete(s, TegraNVDECSubmit, 1);
    if (ret) {
        snprintf(line, size, "N3 VP9 FAIL submit refused (%d, %s)", ret, why ? why : "engine");
        return ret;
    }
    if ((ret = tegra_nvdec_wait(fence))) {
        snprintf(line, size, "N3 VP9 FAIL decode did not finish (%s; see the job line)",
                 ret == -60  ? "past its bound"
                 : ret == -5 ? "done, but its interrupt never came"
                             : "engine refused");
        return ret;
    }
    const uint64_t us = nowUs() - began;
    const uint32_t rows = (kVP9TestHeight + 63) & ~63u;
    tegra_nvdec_cache_flush(input, sizeof(nvdec_status_s));
    tegra_nvdec_cache_flush(gVP9Blocks[3].va, kVP9TestPitch * rows * 3 / 2);
    const nvdec_status_s *status = (const nvdec_status_s *)input;
    uint32_t crc = 0xffffffffu;
    for (uint32_t y = 0; y < kVP9TestHeight; y++)
        for (uint32_t x = 0; x < kVP9TestWidth; x++)
            crc = crc32(crc, gVP9Blocks[3].va[blockLinear(x, y, kVP9TestPitch)]);
    crc = ~crc;
    const bool ok = crc == kVP9TestLumaCrc && !status->error_status;
    snprintf(line, size,
             "N3 VP9 %s %ux%u key frame in %llu us: error_status %#x, %u cycles; luma CRC %#010x (expected %#010x, "
             "libvpx)%s",
             ok ? "ok" : "FAIL", kVP9TestWidth, kVP9TestHeight, us, status->error_status, status->cycle_count, crc,
             kVP9TestLumaCrc, ok ? "" : "; VP9 stays off, H.264 is unaffected");
    return ok ? 0 : -5;
}

// N3 HEVC: x265's 176x144 IDR (CTU 16) through the plug-in's own method stream and buffer layout (HEVCPicture176.h,
// made by SwitchVideoDecoders/test/hevc_host --selftest), luma CRC against FFmpeg's decode. Buffers 1 aux (colocated
// motion, filter), 2 input (status, tile sizes, scaling lists, bitstream), 3 surface; every picture slot names it.
TegraNVDECBlock gHEVCBlocks[4];

bool resolveHEVC(void *, uint32_t handle, TegraNVDECBlock *block)
{
    if (handle < 1 || handle > 3)
        return false;
    *block = gHEVCBlocks[handle];
    return true;
}

int runHEVC(char *line, size_t size)
{
    for (int i = 1; i <= 3; i++) {
        if (tegra_nvdec_buffer_alloc(kHEVCTestBytes[i], &gHEVCBlocks[i])) {
            snprintf(line, size, "N3 HEVC FAIL no NVDEC memory");
            return -12;
        }
    }
    memcpy(gHEVCBlocks[2].va + kHEVCTestInputOff, kHEVCTestInput, sizeof(kHEVCTestInput));
    TegraNVDECSubmit *s = IONewZero(TegraNVDECSubmit, 1);
    if (!s)
        return -12;
    s->codec = TEGRA_NVDEC_CODEC_HEVC;
    s->setup_bytes = sizeof(kHEVCTestSetup);
    memcpy(s->setup, kHEVCTestSetup, sizeof(kHEVCTestSetup));
    s->entry_count = sizeof(kHEVCTestEntries) / sizeof(kHEVCTestEntries[0]);
    memcpy(s->entries, kHEVCTestEntries, sizeof(kHEVCTestEntries));
    s->clean[0] = {2, kHEVCTestInputOff, (uint32_t)sizeof(kHEVCTestInput), 0};
    s->clean_count = 1;

    const uint64_t began = nowUs();
    uint32_t fence = 0;
    const char *why = nullptr;
    int ret = tegra_nvdec_run(s, resolveHEVC, nullptr, &fence, &why);
    IODelete(s, TegraNVDECSubmit, 1);
    if (ret) {
        snprintf(line, size, "N3 HEVC FAIL submit refused (%d, %s)", ret, why ? why : "engine");
        return ret;
    }
    if ((ret = tegra_nvdec_wait(fence))) {
        snprintf(line, size, "N3 HEVC FAIL decode did not finish (%s; see the job line)",
                 ret == -60  ? "past its bound"
                 : ret == -5 ? "done, but its interrupt never came"
                             : "engine refused");
        return ret;
    }
    const uint64_t us = nowUs() - began;
    const uint32_t rows = (kHEVCTestHeight + 63) & ~63u;
    tegra_nvdec_cache_flush(gHEVCBlocks[2].va, sizeof(nvdec_status_s));
    tegra_nvdec_cache_flush(gHEVCBlocks[3].va, kHEVCTestPitch * rows * 3 / 2);
    const nvdec_status_s *status = (const nvdec_status_s *)gHEVCBlocks[2].va;
    uint32_t crc = 0xffffffffu;
    for (uint32_t y = 0; y < kHEVCTestHeight; y++)
        for (uint32_t x = 0; x < kHEVCTestWidth; x++)
            crc = crc32(crc, gHEVCBlocks[3].va[blockLinear(x, y, kHEVCTestPitch)]);
    crc = ~crc;
    const bool ok = crc == kHEVCTestLumaCrc && !status->error_status;
    snprintf(line, size,
             "N3 HEVC %s %ux%u IDR in %llu us: error_status %#x, slice header error %#x, %u cycles; luma CRC %#010x "
             "(expected %#010x, FFmpeg)%s",
             ok ? "ok" : "FAIL", kHEVCTestWidth, kHEVCTestHeight, us, status->error_status,
             status->slice_header_error_code, status->cycle_count, crc, kHEVCTestLumaCrc,
             ok ? "" : "; HEVC stays off, H.264 and VP9 are unaffected");
    return ok ? 0 : -5;
}
}  // namespace

extern "C" int tegra_nvdec_self_test_hevc(void)
{
    char line[288];
    line[0] = 0;
    const int ret = runHEVC(line, sizeof(line));
    IOLog("TegraNVDEC: %s\n", line);
    if (ret != -60)
        for (int i = 1; i <= 3; i++)
            tegra_nvdec_buffer_free(&gHEVCBlocks[i]);
    return ret;
}

extern "C" int tegra_nvdec_self_test_vp9(void)
{
    char line[256];
    line[0] = 0;
    const int ret = runVP9(line, sizeof(line));
    IOLog("TegraNVDEC: %s\n", line);
    if (ret != -60)
        for (int i = 1; i <= 3; i++)
            tegra_nvdec_buffer_free(&gVP9Blocks[i]);
    return ret;
}

extern "C" int tegra_nvdec_self_test(void)
{
    char line[256];
    line[0] = 0;
    const int ret = run(line, sizeof(line));
    IOLog("TegraNVDEC: %s\n", line);
    if (ret != -60)  // a job still in the engine keeps its buffers
        for (int i = kInput; i <= kSurface; i++)
            tegra_nvdec_buffer_free(&gBlocks[i]);
    return ret;
}
