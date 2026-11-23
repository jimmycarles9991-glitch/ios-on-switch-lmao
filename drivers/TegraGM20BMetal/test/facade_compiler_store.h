// SPDX-License-Identifier: MIT
#pragma once
#include "facade_harness.h"
#include "../codegen_request.h"
#include "../compile_request.h"
#include "../compute_facts_entry.h"
#include "../compute_key.h"
#include "../compute_record.h"
#include "../library_entry.h"
#include "../reflection_blob.h"
#include "../../../kits/SwitchSeedPack/SwitchSeedStore.h"
#include "../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace harness {

constexpr uint32_t kServing = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected | TegraGM20BCompilerRegistered;

// The shader compiler as the kernel model runs it for a library: each request kept, and answered with `made` (or, with
// `log` set, the front end's error) in the store under the request's own key and name
struct LibraryCompiler {
    switch_seed_pack::StoreWriter store;
    std::vector<gm20b::CompileRequest> asked;
    std::vector<gm20b::CompileRequest> consumers;
    gm20b::MadeLibrary made;
    std::string log;

    int32_t compute(const gm20b::CompileRequest &r)
    {
        gm20b::CacheKey key;
        key.stack = r.stack;
        key.key = r.key;
        std::string entry;
        if (r.kind == gm20b::RequestKind::Facts) {
            const uint32_t facts[] = {256, 32, 0};
            entry = gm20b::encodeFactsEntry(key, facts, "");
        } else if (gm20b::computeReflection(r)) {
            gm20b::ReflectionHeader header = {};
            memcpy(header.magic, gm20b::kReflectionMagic, sizeof header.magic);
            header.version = gm20b::kReflectionVersion;
            header.stages = gm20b::kReflectionComputeStages;
            const gm20b::ReflectionRange range = {};
            std::string blob(reinterpret_cast<const char *>(&header), sizeof header);
            blob.append(reinterpret_cast<const char *>(&range), sizeof range);
            entry = gm20b::encodeReflectionEntry(key, &blob, "");
        } else if (r.kind == gm20b::RequestKind::Kernel) {
            gm20b::ComputeRecord record;
            record.keyString = r.key.substr(strlen("kernel | "));
            record.kernel.keyHash = gm20b::computeKeyHash(record.keyString);
            record.kernel.regionWord = -1;
            record.kernel.qmd[18] = r.kernel.threads[0] << 16;
            record.kernel.qmd[19] = r.kernel.threads[1] | r.kernel.threads[2] << 16;
            record.code.assign(64, 0);
            record.cbufs = 1;
            record.cbufType[0] = gm20b::CbufRoot;
            entry = gm20b::encodeKernelEntry(key, &record, "");
        } else {
            return TegraGM20BCompileUnavailable;
        }
        consumers.push_back(r);
        std::vector<gm20b::LibraryIdentity> ids;
        for (const auto &library : r.libraries)
            ids.push_back(gm20b::identityOf(library));
        store.put(gm20b::storeName(key, r, ids).data(), reinterpret_cast<const uint8_t *>(entry.data()),
                  uint32_t(entry.size()));
        return TegraGM20BCompileMade;
    }

    bool serve()
    {
        uint8_t identity[switch_seed_pack::kIdentityBytes] = {};
        identity[8] = 0x99;
        std::string why;
        if (!store.open(dir() + "/store", identity, 64, 1u << 20, why))
            return false;
        kernel_model::model.compilerFlags = kServing | TegraGM20BCompilerStore;
        kernel_model::model.store = store.base();
        kernel_model::model.storeBytes = store.mappedBytes();
        kernel_model::model.compiler = [this](const std::string &bytes) {
            gm20b::CompileRequest r;
            if (!gm20b::decodeRequest(bytes, r))
                return int32_t(TegraGM20BCompileUnavailable);
            if (r.kind != gm20b::RequestKind::Library)
                return compute(r);
            asked.push_back(r);
            const gm20b::CacheKey k = gm20b::libraryCacheKey(r.stack, r.codeGenType, r.codeGen);
            if (k.key != r.key || !gm20b::codegen::requestOk(r.codeGenType, r.codeGen))
                return int32_t(TegraGM20BCompileUnavailable);
            const std::string entry = gm20b::encodeLibraryEntry(k, log.empty() ? &made : nullptr, log);
            store.put(gm20b::storeName(k, r, {}).data(), reinterpret_cast<const uint8_t *>(entry.data()),
                      uint32_t(entry.size()), true);
            return int32_t(log.empty() ? TegraGM20BCompileMade : TegraGM20BCompileRefused);
        };
        return true;
    }

    // last: goes first, before what the compiler hook captures, which the device may still call
    kernel_model::HooksCleared cleared;
};

inline id<MTLDevice> completing()
{
    id<MTLDevice> device = facade::device();
    const std::string cache = dir() + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return device;
}

inline bool computeLaunched(size_t firstJob)
{
    for (size_t j = firstJob; j < kernel_model::model.jobs.size(); j++) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        if (count < 0)
            return false;
        for (long i = 0; i < count; i++)
            if (methods[i].subchannel == 1 && methods[i].method == NVB1C0_SEND_PCAS_A)
                return true;
    }
    return false;
}

}  // namespace harness
