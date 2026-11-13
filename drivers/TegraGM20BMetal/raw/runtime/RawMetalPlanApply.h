// SPDX-License-Identifier: MIT
#pragma once
#include "RawMetalPlan.h"

namespace raw {

// Of a table's `count` slots, those worth comparing: the ones edited since the applied snapshot `from`, every one where
// none was applied (kNoSnapshot)
inline uint32_t slotsToCompare(uint32_t edited, uint32_t from, size_t count, StateCursor &cursor)
{
    const uint32_t all = count >= 32 ? ~0u : (1u << count) - 1;
    const uint32_t slots = (from == kNoSnapshot ? ~0u : edited) & all;
    cursor.slotsCompared += uint32_t(__builtin_popcount(slots));
    cursor.slotsInTables += uint32_t(__builtin_popcount(all));
    return slots;
}

template <typename Sink>
void applyBuffers(const BufferSnapshots &table, uint32_t from, uint32_t to, uint32_t stage, StateCursor &cursor,
                  Sink &sink)
{
    const auto bindings = table[to];
    const bool hasOld = from != kNoSnapshot;
    const auto old = table[hasOld ? from : to];
    const uint32_t edited = hasOld ? table.changedFrom(from, to) : ~0u;
    for (uint32_t slots = slotsToCompare(edited, from, bindings.size(), cursor); slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        if (bindings[slot].source != BufferBinding::Source::Unset && (!hasOld || bindings[slot] != old[slot]))
            sink.buffer(stage, slot, bindings[slot]);
    }
}

template <size_t N, typename Apply>
void applyObjects(const ObjectSnapshots<N> &table, uint32_t from, uint32_t to, StateCursor &cursor, Apply apply)
{
    const auto bindings = table[to];
    const bool hasOld = from != kNoSnapshot;
    const auto old = table[hasOld ? from : to];
    const uint32_t edited = hasOld ? table.changedFrom(from, to) : ~0u;
    for (uint32_t slots = slotsToCompare(edited, from, N, cursor) & bindings.set; slots; slots &= slots - 1) {
        const uint32_t slot = uint32_t(__builtin_ctz(slots));
        if (!hasOld || !(old.set & (1u << slot)) || bindings.objects[slot] != old.objects[slot])
            apply(slot, bindings.objects[slot]);
    }
}

// The snapshot's bindings that differ from the applied one's, in the sink, and its raster state where it differs. Only
// the slots the record edited between the two are compared (StateSnapshots::changedFrom).
template <typename Sink>
void applyRenderState(const RMCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink)
{
    if (cursor.render == index)
        return;
    const auto &next = plan.renderStates[index];
    const auto *previous = cursor.render == kNoSnapshot ? nullptr : &plan.renderStates[cursor.render];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    for (uint32_t stage = 0; stage < 2; ++stage) {
        if (!previous || previous->buffers[stage] != next.buffers[stage])
            applyBuffers(plan.buffers[stage], previous ? previous->buffers[stage] : kNoSnapshot, next.buffers[stage],
                         stage + 1, cursor, sink);
        if (!previous || previous->textures[stage] != next.textures[stage])
            applyObjects(plan.textures[stage], previous ? previous->textures[stage] : kNoSnapshot, next.textures[stage],
                         cursor, [&](uint32_t slot, void *object) {
                             sink.texture(stage + 1, slot, object);
                         });
        if (!previous || previous->samplers[stage] != next.samplers[stage])
            applyObjects(plan.samplers[stage], previous ? previous->samplers[stage] : kNoSnapshot, next.samplers[stage],
                         cursor, [&](uint32_t slot, void *object) {
                             sink.sampler(stage + 1, slot, object);
                         });
    }
    if (!previous || previous->raster != next.raster)
        sink.raster(plan.raster[next.raster], previous ? &plan.raster[previous->raster] : nullptr);
    cursor.render = index;
}

template <typename Sink>
void applyKernelState(const RMCommandPlan &plan, uint32_t index, StateCursor &cursor, Sink &sink)
{
    if (cursor.compute == index)
        return;
    const auto &next = plan.kernelStates[index];
    const auto *previous = cursor.compute == kNoSnapshot ? nullptr : &plan.kernelStates[cursor.compute];
    if (previous && previous->epoch != next.epoch)
        previous = nullptr;
    if (!previous || previous->buffers != next.buffers)
        applyBuffers(plan.buffers[2], previous ? previous->buffers : kNoSnapshot, next.buffers, 3, cursor, sink);
    if (!previous || previous->textures != next.textures)
        applyObjects(plan.textures[2], previous ? previous->textures : kNoSnapshot, next.textures, cursor,
                     [&](uint32_t slot, void *object) {
                         sink.texture(3, slot, object);
                     });
    if (!previous || previous->samplers != next.samplers)
        applyObjects(plan.samplers[2], previous ? previous->samplers : kNoSnapshot, next.samplers, cursor,
                     [&](uint32_t slot, void *object) {
                         sink.sampler(3, slot, object);
                     });
    if (!previous || previous->state != next.state)
        sink.kernel(plan.kernel[next.state], previous ? &plan.kernel[previous->state] : nullptr);
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

}  // namespace raw
