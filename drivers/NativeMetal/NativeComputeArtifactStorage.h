// SPDX-License-Identifier: MIT
#pragma once
#include <mutex>
#include "NativeMetalArtifactCache.h"
#include "bridge/NativeMetalComputeArtifact.h"

class NativeComputeArtifactStorage {
public:
    // The process's own directory, read first; then the seed's index and packs, mapped at the first load
    std::string cache, seed;

    NmbComputeArtifact *load(NmbDevice *device, const NmbComputePipelineDescriptor &descriptor, bool reflection,
                             const native_artifact_cache::Key &key) const
    {
        native_artifact_cache::Mapping mapped;
        if (mapped.load(cache, key))
            if (auto *artifact =
                    nmb_compute_artifact_import(device, &descriptor, reflection, mapped.data(), mapped.size()))
                return artifact;
        const void *payload = nullptr;
        size_t bytes = 0;
        if (seeded().find(key, payload, bytes))
            return nmb_compute_artifact_import(device, &descriptor, reflection, payload, bytes);
        return nullptr;
    }

    static bool save(const NmbComputeArtifact *artifact, const native_artifact_cache::Key &key,
                     const std::string &directory)
    {
        struct Write {
            const native_artifact_cache::Key &key;
            const std::string &directory;
        } context{key, directory};
        return nmb_compute_artifact_serialize(
            artifact,
            [](const void *bytes, size_t size, void *opaque) {
                const auto &out = *static_cast<const Write *>(opaque);
                return native_artifact_cache::store(out.directory, out.key, bytes, size);
            },
            &context);
    }

private:
    mutable std::once_flag opened_;
    mutable native_artifact_cache::Seed packed_;

    const native_artifact_cache::Seed &seeded() const
    {
        std::call_once(opened_, [this] {
            if (!seed.empty())
                packed_.open(seed);
        });
        return packed_;
    }
};
