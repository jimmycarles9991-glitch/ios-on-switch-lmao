// SPDX-License-Identifier: MIT
#pragma once
#include "TegraGM20BMetalPlan.h"
#include "stats.h"

namespace gm20b {

// Of a table's `count` slots, those worth comparing: the ones edited since the applied snapshot `from`, every one where
// none was applied (kNoSnapshot)
template <typename Mask> Mask slotsToCompare(const Mask &edited, uint32_t from, size_t count, StateCursor &cursor)
{
    const Mask all = firstSlots<Mask>(count);
    const Mask slots = (from == kNoSnapshot ? everySlot<Mask>() : edited) & all;
#if SWITCH_METAL_PROF
    cursor.slotsCompared += slotCount(slots);
    cursor.slotsInTables += slotCount(all);
#else
    (void)cursor;
#endif
    return slots;
}

template <typename Sink>
void applyBuffers(const BufferSnapshots &table, uint32_t from, uint32_t to, uint32_t stage, StateCursor &cursor,
                  Sink &sink, bool own = false)
{
    const auto *chunks = table.chunksData(own);
    const auto *headers = table.headersData(own);
    const auto &toHeader = headers[to];
    const bool hasOld = from != kNoSnapshot;
    const auto &fromHeader = headers[hasOld ? from : to];
    const uint32_t edited = hasOld ? table.changedFrom(from, to, own) : ~0u;
    for (uint32_t slots = slotsToCompare(edited, from, 31, cursor); slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        const auto &cur = chunks[toHeader[slot >> 2]][slot & 3];
        if (cur.source != BufferBinding::Source::Unset && (!hasOld || cur != chunks[fromHeader[slot >> 2]][slot & 3]))
            sink.buffer(stage, slot, cur);
    }
}

template <size_t N, typename Apply>
void applyObjects(const ObjectSnapshots<N> &table, uint32_t from, uint32_t to, StateCursor &cursor, Apply apply,
                  bool own = false)
{
    constexpr size_t K = N <= 32 ? 4 : 8;
    const auto *chunks = table.chunksData(own);
    const auto *headers = table.headersData(own);
    const auto *sets = table.setsData(own);
    const auto &toHeader = headers[to];
    const auto toSet = sets[to];
    const bool hasOld = from != kNoSnapshot;
    const auto &fromHeader = headers[hasOld ? from : to];
    const auto fromSet = sets[hasOld ? from : to];
    using Mask = typename ObjectSnapshots<N>::Mask;
    const Mask edited = hasOld ? table.changedFrom(from, to, own) : everySlot<Mask>();
    forEachSlot(slotsToCompare(edited, from, N, cursor) & toSet, [&](uint32_t slot) {
        void *cur = chunks[toHeader[slot / K]][slot % K];
        if (!hasOld || !hasSlot(fromSet, slot) || cur != chunks[fromHeader[slot / K]][slot % K])
            apply(slot, cur);
    });
}

// The snapshot's bindings that differ from the applied one's, in the sink, and its raster state where it differs. Only
// the slots the record edited between the two are compared (StateSnapshots::changedFrom).
template <typename Sink>
void applyRenderState(const SwitchMetalCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink,
                      bool own = false)
{
    if (cursor.render == index)
        return;
    const auto *states = own ? plan.renderStates.ownData() : plan.renderStates.data();
    const auto &next = states[index];
    const auto *previous = cursor.render == kNoSnapshot ? nullptr : &states[cursor.render];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!previous || previous->buffers[stage] != next.buffers[stage])
            applyBuffers(plan.buffers[stage], previous ? previous->buffers[stage] : kNoSnapshot, next.buffers[stage],
                         stage + 1, cursor, sink, own);
        if (!previous || previous->textures[stage] != next.textures[stage])
            applyObjects(
                plan.textures[stage], previous ? previous->textures[stage] : kNoSnapshot, next.textures[stage], cursor,
                [&](uint32_t slot, void *object) {
                    sink.texture(stage + 1, slot, object);
                },
                own);
        if (!previous || previous->samplers[stage] != next.samplers[stage])
            applyObjects(
                plan.samplers[stage], previous ? previous->samplers[stage] : kNoSnapshot, next.samplers[stage], cursor,
                [&](uint32_t slot, void *object) {
                    sink.sampler(stage + 1, slot, object);
                },
                own);
    }
    if (!previous || previous->raster != next.raster) {
        const auto *rasters = plan.raster.valuesData(own);
        sink.raster(rasters[next.raster], previous ? &rasters[previous->raster] : nullptr);
    }
    cursor.render = index;
}

template <typename Sink>
void applyKernelState(const SwitchMetalCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink,
                      bool own = false)
{
    if (cursor.compute == index)
        return;
    const auto *states = own ? plan.kernelStates.ownData() : plan.kernelStates.data();
    const auto &next = states[index];
    const auto *previous = cursor.compute == kNoSnapshot ? nullptr : &states[cursor.compute];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    if (!previous || previous->buffers != next.buffers)
        applyBuffers(plan.buffers[2], previous ? previous->buffers : kNoSnapshot, next.buffers, 3, cursor, sink, own);
    if (!previous || previous->textures != next.textures)
        applyObjects(
            plan.kernelTextures, previous ? previous->textures : kNoSnapshot, next.textures, cursor,
            [&](uint32_t slot, void *object) {
                sink.texture(3, slot, object);
            },
            own);
    if (!previous || previous->samplers != next.samplers)
        applyObjects(
            plan.samplers[2], previous ? previous->samplers : kNoSnapshot, next.samplers, cursor,
            [&](uint32_t slot, void *object) {
                sink.sampler(3, slot, object);
            },
            own);
    if (!previous || previous->state != next.state) {
        const auto *kernels = plan.kernel.valuesData(own);
        sink.kernel(kernels[next.state], previous ? &kernels[previous->state] : nullptr);
    }
    cursor.compute = index;
}

template <typename T>
bool rasterChanged(const RasterState &next, const RasterState *previous, RasterState::Field field,
                   T RasterState::*member)
{
    return (next.set & field) && (!previous || !(previous->set & field) || next.*member != previous->*member);
}

inline float stateFloat(uint32_t value)
{
    float result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

inline double stateDouble(uint64_t value)
{
    double result;
    std::memcpy(&result, &value, sizeof result);
    return result;
}

}  // namespace gm20b
