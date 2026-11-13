// SPDX-License-Identifier: MIT
// raw_direct_encode.h against NVK's exports over the corpus: testdata/direct_encode_vectors.txt holds each distinct input a
// raw pack's direct compile asked NVK about (sampler headers, texture header word 0, set-0 slots and sizes, buffer
// descriptor sizes, compute and split QMDs) with NVK's answer, as x1-nomvk-shadow's packs recorded them for 21H461 and
// 24B119. Every answer must come out word for word.
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "../raw/runtime/raw_direct_encode.h"

namespace {

std::string hexWords(const uint32_t *w, size_t n)
{
    std::string out;
    char b[12];
    for (size_t i = 0; i < n; i++) {
        snprintf(b, sizeof b, "%s%08x", i ? "," : "", w[i]);
        out += b;
    }
    return out;
}

std::vector<NmbDirectBinding> bindingsOf(const std::string &text)
{
    std::vector<NmbDirectBinding> out;
    if (text == "-")
        return out;
    std::istringstream in(text);
    for (std::string item; std::getline(in, item, ';');) {
        NmbDirectBinding b = {};
        sscanf(item.c_str(), "%u:%d:%u:%u", &b.binding, &b.type, &b.count, &b.flags);
        out.push_back(b);
    }
    return out;
}

NmbDirectSamplerState samplerOf(const std::string &text)
{
    NmbDirectSamplerState s = {};
    int anisotropy = 0, compare = 0, unnormalized = 0;
    sscanf(text.c_str(), "%u,%u,%u,%u,%u,%u,%a,%a,%a,%d,%a,%d,%u,%u,%u,%u,%u,%u,%u,%d", &s.magFilter, &s.minFilter,
           &s.mipmapMode, &s.address[0], &s.address[1], &s.address[2], &s.mipLodBias, &s.minLod, &s.maxLod, &anisotropy,
           &s.maxAnisotropy, &compare, &s.compareOp, &s.borderColor, &s.reduction, &s.borderComponents[0],
           &s.borderComponents[1], &s.borderComponents[2], &s.borderComponents[3], &unnormalized);
    s.anisotropy = anisotropy, s.compare = compare, s.unnormalized = unnormalized;
    return s;
}

// The fields shadow::qmdInputs names: gprs, barriers, local size, smem, slm, crs, shader data, then each cbuf's type:set
raw::NvkStage stageOf(const std::string &text)
{
    raw::NvkStage s = {};
    unsigned gprs, bar, x, y, z, smem, slm, crs, data;
    sscanf(text.c_str(), "gprs=%u,bar=%u,local=%u:%u:%u,smem=%u,slm=%u,crs=%u,data=%u", &gprs, &bar, &x, &y, &z, &smem,
           &slm, &crs, &data);
    s.numGprs = uint8_t(gprs), s.numBarriers = uint8_t(bar);
    s.localSize[0] = uint16_t(x), s.localSize[1] = uint16_t(y), s.localSize[2] = uint16_t(z);
    s.smemSize = uint16_t(smem), s.slmSize = slm, s.crsSize = crs, s.dataBytes = data;
    for (size_t at = text.find(",cb"); at != std::string::npos; at = text.find(",cb", at + 3)) {
        unsigned c, type, set;
        if (sscanf(text.c_str() + at, ",cb%u=%u:%u", &c, &type, &set) == 3 && c < 16) {
            s.cbufTypes[c] = uint8_t(type), s.cbufSets[c] = uint8_t(set);
            s.cbufCount = c + 1;
        }
    }
    return s;
}

std::string answer(const std::string &kind, const std::string &input)
{
    if (kind == "tsc") {
        uint32_t w[8];
        return raw::samplerHeader(samplerOf(input), w) ? hexWords(w, 8) : "-";
    }
    if (kind == "tic0") {
        unsigned format, c[4];
        char use[16] = {};
        sscanf(input.c_str(), "%u %u,%u,%u,%u %15s", &format, &c[0], &c[1], &c[2], &c[3], use);
        const uint32_t components[4] = {c[0], c[1], c[2], c[3]};
        uint32_t word = 0;
        return raw::viewWord0(format, components, !strcmp(use, "storage"), word) ? hexWords(&word, 1) : "-";
    }
    if (kind == "slot") {
        const size_t at = input.rfind(" b=");
        const std::vector<NmbDirectBinding> b = bindingsOf(input.substr(0, at));
        raw::SetSlot slot;
        if (!raw::Set0(b.data(), uint32_t(b.size())).slot(uint32_t(std::stoul(input.substr(at + 3))), slot))
            return "-";
        return std::to_string(slot.offset) + "+" + std::to_string(slot.stride);
    }
    if (kind == "setbytes") {
        const std::vector<NmbDirectBinding> b = bindingsOf(input);
        return std::to_string(b.empty() ? 0 : uint16_t(raw::Set0(b.data(), uint32_t(b.size())).bytes()));
    }
    if (kind == "bufsize") {
        int type;
        unsigned long long range;
        sscanf(input.c_str(), "%d %llu", &type, &range);
        return std::to_string(raw::bufferDescriptorBytes(type, range));
    }
    uint32_t q[64] = {};
    if (kind == "qmd") {
        const size_t at = input.rfind(" set=");
        return raw::computeQmd(stageOf(input.substr(0, at)), uint32_t(std::stoul(input.substr(at + 5))), 0, q)
                   ? hexWords(q, 64)
                   : "-";
    }
    if (kind == "splitqmd")
        return raw::splitQmd(stageOf(input), q) ? hexWords(q, 64) : "-";
    return "unknown kind " + kind;
}

TEST(DirectEncode, AnswersAsNvk)
{
    std::istringstream text(switch_ios::test::SourceText("testdata/direct_encode_vectors.txt"));
    unsigned checked = 0;
    for (std::string line; std::getline(text, line);) {
        if (line.rfind("SHADOW ", 0) != 0)
            continue;
        const size_t kindEnd = line.find(' ', 7), eq = line.rfind(" = ");
        ASSERT_NE(eq, std::string::npos) << line;
        const std::string kind = line.substr(7, kindEnd - 7), input = line.substr(kindEnd + 1, eq - kindEnd - 1);
        EXPECT_EQ(answer(kind, input), line.substr(eq + 3)) << line;
        checked++;
    }
    EXPECT_GT(checked, 0u);
}

// Set 1 as metal2vulkan's heap_set makes it on the raw path's compile devices (pipeline/link.rs: binding n per heap
// class, 8192 elements, PARTIALLY_BOUND | UPDATE_AFTER_BIND, UPDATE_UNUSED_WHILE_PENDING on some), live and offline
// alike
constexpr uint32_t kHeapFlags = 0x4 | 0x1, kPendingFlag = 0x2, kUpdateAfterBindPool = 0x2;
constexpr int32_t kSampledImage = 2, kSamplerType = 0, kStorageImage = 3, kStorageBuffer = 7;

NmbDirectBinding binding(uint32_t number, int32_t type, uint32_t count, uint32_t flags)
{
    NmbDirectBinding b = {};
    b.binding = number, b.type = type, b.count = count, b.flags = flags;
    return b;
}

struct HeapLayout {
    std::vector<NmbDirectBinding> set0 = {binding(0, kStorageBuffer, 1, 0)}, heap;
    NmbDirectSet sets[2] = {};
    NmbDirectLayout layout = {};

    explicit HeapLayout(std::vector<NmbDirectBinding> h) : heap(std::move(h))
    {
        sets[0] = {set0.data(), uint32_t(set0.size()), 0};
        sets[1] = {heap.data(), uint32_t(heap.size()), kUpdateAfterBindPool};
        layout = {sets, 2, nullptr, 0};
    }
};

void expectHeap(const HeapLayout &h, const std::vector<raw::PackHeapArray> &want, uint32_t wantBytes)
{
    uint32_t classes[8] = {};
    for (size_t a = 0; a < want.size(); a++)
        classes[a] = want[a].heapClass;
    raw::PackHeapArray arrays[raw::kPackHeapArrays] = {};
    uint32_t bytes = 0;
    ASSERT_EQ(raw::heapSetArrays(&h.layout, uint32_t(want.size()), classes, arrays, bytes), nullptr);
    for (size_t a = 0; a < want.size(); a++) {
        EXPECT_EQ(arrays[a].heapClass, want[a].heapClass) << a;
        EXPECT_EQ(arrays[a].offset, want[a].offset) << a;
    }
    EXPECT_EQ(bytes, wantBytes);
}

// The heap arrays and bytes NVK gave the argument-buffer records of x1-argbuffers3's HW Test pack (iOS 27, r10):
// argument_textures_fragment's and argument_array_fragment's state, and argument_kernel's
TEST(DirectEncode, HeapSetAsArgumentRecords)
{
    expectHeap(HeapLayout({binding(0, kSampledImage, 8192, kHeapFlags), binding(1, kSamplerType, 8192, kHeapFlags)}),
               {{5, 0}, {2, 0x8000}}, 0x10000);
    expectHeap(HeapLayout({binding(0, kSampledImage, 8192, kHeapFlags),
                           binding(1, kSampledImage, 8192, kHeapFlags | kPendingFlag),
                           binding(2, kStorageImage, 8192, kHeapFlags | kPendingFlag)}),
               {{5, 0}, {133, 0x8000}, {261, 0x10000}}, 0x18000);
}

TEST(DirectEncode, HeapSetOffTheRawHeapRefused)
{
    const uint32_t classes[8] = {5, 9};
    raw::PackHeapArray arrays[raw::kPackHeapArrays] = {};
    uint32_t bytes = 0;
    // a device without the raw heap's elements (metal2vulkan's TABLE_CAPACITY), and a heap of buffers
    const HeapLayout unsized(
        {binding(0, kSampledImage, 1u << 20, kHeapFlags), binding(1, kSamplerType, 1u << 20, kHeapFlags)});
    EXPECT_NE(raw::heapSetArrays(&unsized.layout, 2, classes, arrays, bytes), nullptr);
    const HeapLayout buffers(
        {binding(0, kStorageBuffer, 8192, kHeapFlags), binding(1, kSamplerType, 8192, kHeapFlags)});
    EXPECT_NE(raw::heapSetArrays(&buffers.layout, 2, classes, arrays, bytes), nullptr);
    const HeapLayout sized({binding(0, kSampledImage, 8192, kHeapFlags), binding(1, kSamplerType, 8192, kHeapFlags)});
    const uint32_t unworded[8] = {5, 0};
    EXPECT_NE(raw::heapSetArrays(&sized.layout, 2, unworded, arrays, bytes), nullptr);
    NmbDirectLayout setZeroOnly = sized.layout;
    setZeroOnly.setCount = 1;
    EXPECT_NE(raw::heapSetArrays(&setZeroOnly, 2, classes, arrays, bytes), nullptr);
}

// A kernel's set-1 constant buffer is sized as set 0's would be at the heap's bytes, and refused without them
TEST(DirectEncode, HeapSetQmd)
{
    raw::NvkStage heap = stageOf("gprs=16,bar=0,local=64:1:1,smem=0,slm=0,crs=0,data=0,cb0=1:0,cb1=11:1");
    raw::NvkStage set0 = stageOf("gprs=16,bar=0,local=64:1:1,smem=0,slm=0,crs=0,data=0,cb0=1:0,cb1=3:0");
    uint32_t a[64] = {}, b[64] = {};
    ASSERT_TRUE(raw::computeQmd(heap, 0x100, 0x8000, a));
    ASSERT_TRUE(raw::computeQmd(set0, 0x8000, 0, b));
    EXPECT_EQ(hexWords(a, 64), hexWords(b, 64));
    EXPECT_FALSE(raw::computeQmd(heap, 0x100, 0, a));
    raw::NvkStage set2 = stageOf("gprs=16,bar=0,local=64:1:1,smem=0,slm=0,crs=0,data=0,cb0=1:0,cb1=19:2");
    EXPECT_FALSE(raw::computeQmd(set2, 0x100, 0x8000, a));
}

}  // namespace
