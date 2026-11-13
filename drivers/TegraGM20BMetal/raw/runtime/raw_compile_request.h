// SPDX-License-Identifier: MIT
// A compile request, as a GPU client hands it to the system's shader compiler (drivers/TegraGM20BMetal/compiler) through the
// kernel: a render state's descriptor fields, or a kernel's limits and dispatch, each stage's function and the library it
// comes from, and the variant. It carries the key the client made, which the compiler checks against the key it makes itself
// from the rest (RawCompileRequest.mm), and no record. A record is filed under a store name that binds that key to the exact
// libraries it was compiled from: a system volume's library by its path (the compiler reads it there), any other by the
// SHA-256 of its bytes (which the request carries). A client only ever finds a record made from what it holds itself.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "bridge/NativeMetalDirect.h"
#include "raw_cache.h"
#include "raw_cubic.h"
#include "raw_pipeline_fields.h"

namespace raw {

constexpr uint64_t kRequestLimit = uint64_t(256) << 20;  // TegraGM20BCompilerABI.h TegraGM20BCompileRequestLimit
// A state's stages and libraries are bounded by the request's bytes alone: these only keep a decode's counts sane, above any
// state Metal makes (its linked functions are unbounded). Bindings are Metal's own per-stage limit.
constexpr uint32_t kRequestStageLimit = 1u << 16, kRequestLibraryLimit = 1u << 16, kRequestBindLimit = 31;

using Digest = std::array<uint8_t, 32>;

enum class RequestKind : uint8_t {
    Render,
    Kernel
};

// Primary: the state's record; Split: the same with its fragment program's split, which the compiler makes after the
// primary record is published and nobody waits for
enum class RequestForm : uint8_t {
    Primary,
    Split
};

struct RequestLibrary {
    std::string path;     // where the client loaded it from ("": made from data)
    bool system = false;  // systemLibraryPath(path): the compiler reads it there, and `bytes` is empty
    std::string bytes;
};

// A stage's function: for a render state the vertex function, the fragment function, then the linked ones (tags "vl",
// "vp", "fl", "fp" as RawMetalLibrary.mm's linkedKey lists them); for a kernel its one function (tag "k")
struct RequestStage {
    std::string tag;
    std::string function;      // the library function it was made from
    std::string constants;     // its constants' text (RawPipelineKey.h constantsText)
    std::string name;          // the name it was made under (its specialized name, else `function`)
    bool specialized = false;  // made with constant values (newFunctionWithName:constantValues:, even with none)
    uint32_t library = 0;
};

// A kernel at one dispatch shape and its bindings (RawDirect.h KernelDispatch)
struct KernelRequest {
    uint32_t maxThreads = 0;
    bool multipleOfWidth = false, regionMode = false;
    uint32_t threads[3] = {};
    uint32_t memory[kRequestBindLimit] = {};
    std::vector<NmbDirectBoundTexture> textures;
    std::vector<NmbDirectBoundSampler> samplers;

    NmbDirectDispatch dispatch() const;
};

struct CompileRequest {
    uint64_t stack = 0;
    RequestKind kind = RequestKind::Render;
    RequestForm form = RequestForm::Primary;
    PipelineFields fields;
    std::vector<RequestStage> stages;
    std::vector<RequestLibrary> libraries;
    CubicBinds cubic;
    uint8_t fragmentTests = 0;
    KernelRequest kernel;
    std::string key;  // the CacheKey text the client made (a kernel's starts "kernel | ")
};

std::string encodeRequest(const CompileRequest &r);
// False: not a request this build reads (a bound passed, a stage naming no library, a system library given bytes)
bool decodeRequest(const std::string &bytes, CompileRequest &out);

// What a library is to the store name: a system library's path, else its bytes
struct LibraryIdentity {
    bool system = false;
    Digest digest{};
};

Digest sha256(const void *bytes, size_t size);
LibraryIdentity identityOf(const RequestLibrary &library);
// The store's name of a program slot's bytes (raw_record.h programSlot), apart from every record's name
Digest programSlotName(const void *slot, size_t size);

// The store's name of the record of `k` made from `r`'s stages in these libraries, and of `r`'s fields and dispatch beyond
// what the key names
Digest storeName(const CacheKey &k, const CompileRequest &r, const std::vector<LibraryIdentity> &libraries);

// The CacheKey of a kernel's compute key (RawComplete.mm kernel())
CacheKey kernelCacheKey(uint64_t stack, const std::string &computeKey);

}  // namespace raw
