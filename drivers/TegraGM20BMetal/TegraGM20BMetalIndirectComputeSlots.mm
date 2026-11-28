// SPDX-License-Identifier: MIT
// A buffer of dispatches' memory (TegraGM20BMetalIndirectCompute.h): the blocks its commands' regions lie in, each command's
// slot and bind slot and their growth, and the commands of a buffer that inherits the pipeline written for one pipeline
#import "TegraGM20BMetalIndirectCompute.h"

namespace {

// A block of `bytes` in `chunks`, a chunk added where the last has no room
std::shared_ptr<SwitchMetalICBBlock> blockIn(std::vector<gm20b::Buffer> &chunks, uint64_t &used, uint32_t bytes)
{
    constexpr uint64_t kChunk = 256 << 10;
    if (chunks.empty() || used + bytes > chunks.back().length) {
        gm20b::Buffer chunk;
        if (!gRM->raw->createBuffer(std::max<uint64_t>(kChunk, bytes), chunk, true))
            return nullptr;
        chunks.push_back(chunk);
        used = 0;
    }
    const gm20b::Buffer &chunk = chunks.back();
    auto block = std::make_shared<SwitchMetalICBBlock>(SwitchMetalICBBlock{chunk.cpu + used, chunk.va + used, bytes});
    used += (bytes + gm20b::icb::kComputeBlockAlign - 1) & ~uint64_t(gm20b::icb::kComputeBlockAlign - 1);
    return block;
}

// Command `c` at `index` into its slot and, where its buffer inherits the encoder's buffers, its bind slot
void writeCommand(uint32_t *slot, uint32_t slotWords, uint32_t *bind, uint32_t bindWords, NSUInteger index,
                  const SwitchMetalICBDispatch &c)
{
    if (c.raw)
        gm20b::icb::dispatch(slot, uint32_t(index), c.words, slotWords);
    else
        gm20b::icb::computeEmpty(slot, slotWords);
    if (bind && (!c.raw || !gm20b::icb::binds(bind, c.binds.data(), uint32_t(c.binds.size()), c.strideWords.data(),
                                              uint32_t(c.strideWords.size()), c.roots.data(), uint32_t(c.roots.size()),
                                              bindWords)))
        gm20b::icb::bindsEmpty(bind, bindWords);
}

// `memory` of `count` slots of `words` made `want` words each where that is more; the old memory into `retired`
bool grown(gm20b::Buffer &memory, uint32_t &words, uint64_t want, uint64_t count, std::vector<gm20b::Buffer> &retired)
{
    if (want <= words)
        return true;
    gm20b::Buffer more;
    if (want > UINT32_MAX || !gRM->raw->createBuffer(count * want * 4, more, true))
        return false;
    retired.push_back(memory);
    memory = more;
    words = uint32_t(want);
    return true;
}

}  // namespace

std::shared_ptr<SwitchMetalICBBlock> gm20bNewBlock(SwitchMetalIndirectComputeBuffer *b, uint32_t bytes)
{
    return blockIn(b->_chunks, b->_chunkUsed, bytes);
}

uint32_t *gm20bComputeSlot(SwitchMetalIndirectComputeBuffer *b, NSUInteger index)
{
    b->_raw.markCpuDirty();
    return reinterpret_cast<uint32_t *>(b->_raw.cpu) + index * b->_slotWords;
}

uint32_t *gm20bBindSlot(SwitchMetalIndirectComputeBuffer *b, NSUInteger index)
{
    b->_binds.markCpuDirty();
    return reinterpret_cast<uint32_t *>(b->_binds.cpu) + index * b->_bindWords;
}

uint64_t gm20bSlotWords(const SwitchMetalICBDispatch &c)
{
    return c.raw ? gm20b::icb::computeSlotWords(c.words.qmd.size()) : gm20b::icb::kComputeSlotWords;
}

uint64_t gm20bBindWords(const SwitchMetalICBDispatch &c)
{
    return c.raw ? gm20b::icb::computeBindSlotWords(c.binds.size(), c.strideWords.size(), c.roots.size())
                 : gm20b::icb::kComputeBindSlotWords;
}

void gm20bWriteWords(SwitchMetalIndirectComputeBuffer *b, NSUInteger index)
{
    writeCommand(gm20bComputeSlot(b, index), b->_slotWords, b->_inheritBuffers ? gm20bBindSlot(b, index) : nullptr,
                 b->_bindWords, index, b->_dispatches[index]);
}

bool gm20bFitSlots(SwitchMetalIndirectComputeBuffer *b, uint64_t slotWords, uint64_t bindWords)
{
    const uint32_t slotsWere = b->_slotWords, bindsWere = b->_bindWords;
    const bool fit = grown(b->_raw, b->_slotWords, slotWords, b->_count, b->_retired) &&
                     (!b->_inheritBuffers || grown(b->_binds, b->_bindWords, bindWords, b->_count, b->_retired));
    if (b->_slotWords != slotsWere || b->_bindWords != bindsWere)
        for (NSUInteger i = 0; i < b->_count; i++)
            gm20bWriteWords(b, i);
    return fit;
}

SwitchMetalICBVariant::~SwitchMetalICBVariant()
{
    for (gm20b::Buffer &chunk : chunks)
        gRM->raw->destroyBuffer(chunk);
    if (slots.va)
        gRM->raw->destroyBuffer(slots);
    if (binds.va)
        gRM->raw->destroyBuffer(binds);
}

std::shared_ptr<SwitchMetalICBVariant>
gm20bPipelineVariant(SwitchMetalIndirectComputeBuffer *b, SwitchMetalComputeState *pipeline,
                     const std::vector<std::shared_ptr<SwitchMetalKernelSet>> &kernels,
                     const std::vector<SwitchMetalICBUnmade> &unmade, const char **why)
{
    auto v = std::make_shared<SwitchMetalICBVariant>();
    v->generation = b->_generation;
    v->missing.assign(b->_count, SwitchMetalICBUnmade{});
    const uint64_t count = b->_count;
    std::vector<SwitchMetalICBDispatch> made(count);
    uint64_t slotWords = gm20b::icb::kComputeSlotWords, bindWords = gm20b::icb::kComputeBindSlotWords;
    for (NSUInteger i = 0; i < count; i++) {
        const SwitchMetalICBDispatch &c = b->_dispatches[i];
        const SwitchMetalKernelSet *k = i < kernels.size() ? kernels[i].get() : nullptr;
        if (!c.dispatched)
            continue;
        if (!k) {
            if (i < unmade.size())
                v->missing[i] = unmade[i];
            continue;
        }
        const gm20b::ComputeDispatch shape =
            gm20b::computeDispatch(c.grid, c.threads, c.perThread, pipeline->_threadLimit);
        const uint64_t bytes = gm20bCommandBytes(*k, shape);
        const std::shared_ptr<SwitchMetalICBBlock> block =
            bytes <= UINT32_MAX ? blockIn(v->chunks, v->chunkUsed, uint32_t(bytes)) : nullptr;
        if (!block) {
            *why = "no memory for an indirect dispatch command's regions at the encoder's pipeline";
            return nullptr;
        }
        gm20b::ComputeEncoder::IcbBuffer buffers[31];
        if (!b->_inheritBuffers)
            gm20bCommandBuffers(c, buffers);
        SwitchMetalICBDispatch &m = made[i];
        if (!gm20bCommandRegions(*k, shape, c, buffers, c.strides, *block, m.words, why) ||
            (b->_inheritBuffers && !gm20bCommandBinds(*k, shape, *block, m, why))) {
            v->missing[i] = SwitchMetalICBUnmade{*why};
            continue;
        }
        m.words.barrier = c.barrier;
        m.raw = true;
        slotWords = std::max(slotWords, gm20bSlotWords(m));
        bindWords = std::max(bindWords, gm20bBindWords(m));
        v->bindSlots |= m.bindSlots;
        v->strideSlots |= m.strideSlots;
    }
    if (slotWords > UINT32_MAX || bindWords > UINT32_MAX ||
        !gRM->raw->createBuffer(count * slotWords * 4, v->slots, true) ||
        (b->_inheritBuffers && !gRM->raw->createBuffer(count * bindWords * 4, v->binds, true))) {
        *why = "no memory for an indirect command buffer's commands at the encoder's pipeline";
        return nullptr;
    }
    v->slotWords = uint32_t(slotWords);
    v->bindWords = uint32_t(bindWords);
    for (NSUInteger i = 0; i < count; i++)
        writeCommand(reinterpret_cast<uint32_t *>(v->slots.cpu) + i * slotWords, v->slotWords,
                     b->_inheritBuffers ? reinterpret_cast<uint32_t *>(v->binds.cpu) + i * bindWords : nullptr,
                     v->bindWords, i, made[i]);
    return v;
}
