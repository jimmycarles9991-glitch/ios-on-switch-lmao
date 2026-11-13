// SPDX-License-Identifier: MIT
// A range replayed into m2v (RawMetalReplay.mm) needs the twin of every attachment of every pass in it. m2v takes a pass with
// a nil attachment and then refuses its draws (AttachmentFormat, NoPipelineState) after the passes before it went out, so a
// range with one missing fails whole, before any of it is encoded.
#pragma once
#include <cstddef>
#include <cstdint>

namespace raw {

// The first object `attachments(at, visit)` names for an operation of [from, to) that `hasTwin` refuses; null when every one
// has a twin. A null attachment is no attachment.
template <typename Attachments, typename HasTwin>
void *missingTwin(size_t from, size_t to, Attachments &&attachments, HasTwin &&hasTwin)
{
    void *missing = nullptr;
    for (size_t at = from; at < to && !missing; ++at)
        attachments(at, [&](void *object) {
            if (!missing && object && !hasTwin(object))
                missing = object;
        });
    return missing;
}

enum class ReplayUse : uint8_t {
    Other,
    Heap,              // a facade heap, which no twin names
    WrittenArguments,  // a buffer an argument encoder wrote: the raw device's resource IDs and addresses
};

// What a range reads that no m2v twin can: the first buffer of `uses(visit)` that `kind` calls written arguments, else its
// first heap where one of the states `states(visit)` names reads argument buffers (`readsArguments`); null: none. A heap
// whose resources the range binds at slots or as attachments draws on m2v through their twins.
template <typename Uses, typename Kind, typename States, typename ReadsArguments>
void *rawArgumentsIn(Uses &&uses, Kind &&kind, States &&states, ReadsArguments &&readsArguments)
{
    void *written = nullptr, *heap = nullptr;
    uses([&](void *object) {
        const ReplayUse k = kind(object);
        if (!written && k == ReplayUse::WrittenArguments)
            written = object;
        else if (!heap && k == ReplayUse::Heap)
            heap = object;
    });
    if (written || !heap)
        return written;
    bool reads = false;
    states([&](void *state) {
        reads = reads || readsArguments(state);
    });
    return reads ? heap : nullptr;
}

}  // namespace raw
