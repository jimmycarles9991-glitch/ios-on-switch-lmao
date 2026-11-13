// SPDX-License-Identifier: MIT
// m2v's ArgumentTable (metal2vulkan lower/push.rs): the push fields a stage's function places past its push window, which
// the function reads from a uniform buffer of set 0 (EntrySpillTable, KernelSpillTable). The raw path fills that buffer
// itself: the record's words with each spilled bind-offset residual and a fragment table's dynamic words in theirs,
// uploaded again only where one of them moved. A record's word index past the root's push words (bufferResidualWord,
// kernelResidualWord, a dynamic field's word - 1) is a word of the field's own stage's table.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace raw {

constexpr uint32_t kRootPushWords = 32, kSpillWordsMax = 255, kSpillResidualWords = 31;

inline bool spilledWord(int word)
{
    return word >= int(kRootPushWords);
}

// The residuals a stage's spilled bind offsets hold, by table word: `mask` bit i, word i holds one
struct SpillResiduals {
    std::array<uint32_t, kSpillResidualWords> word{};
    uint32_t mask = 0;

    void set(int recordWord, uint32_t residual)
    {
        const uint32_t i = uint32_t(recordWord) - kRootPushWords;
        if (i >= kSpillResidualWords)
            return;
        word[i] = residual;
        mask |= 1u << i;
    }
};

// A stage's table for a draw or dispatch: the record's `count` words with `residuals` in theirs
inline void composeSpill(const uint32_t *record, uint32_t count, const SpillResiduals &residuals, uint32_t *out)
{
    memcpy(out, record, count * sizeof(uint32_t));
    for (uint32_t m = residuals.mask; m; m &= m - 1)
        if (const uint32_t i = uint32_t(__builtin_ctz(m)); i < count)
            out[i] = residuals.word[i];
}

// The bytes a table's uniform buffer descriptor names: NVK rounds a uniform range up to the cbuf alignment
constexpr uint32_t spillRange(uint32_t count)
{
    return (count * 4 + 255) & ~255u;
}

// The table a stage's draws read: uploaded again only where its words differ from the upload the encoder made last. Its
// uploads stay live and unchanged until every draw of the encoder retired (raw_descriptors.h DescriptorSnapshot).
class SpillUpload {
public:
    template <typename Upload> uint64_t resolve(const uint32_t *words, uint32_t count, Upload upload)
    {
        if (va_ && count_ == count && !memcmp(words_.data(), words, count * sizeof(uint32_t)))
            return va_;
        const uint64_t va = upload(words, count);
        if (!va)
            return 0;
        memcpy(words_.data(), words, count * sizeof(uint32_t));
        count_ = count;
        va_ = va;
        return va_;
    }

    uint64_t va() const
    {
        return va_;
    }

private:
    std::array<uint32_t, kSpillWordsMax> words_{};
    uint32_t count_ = 0;
    uint64_t va_ = 0;
};

inline void spillDescriptor(uint64_t va, uint32_t count, uint32_t *at)
{
    at[0] = uint32_t(va);
    at[1] = uint32_t(va >> 32);
    at[2] = spillRange(count);
    at[3] = 0;
}

}  // namespace raw
