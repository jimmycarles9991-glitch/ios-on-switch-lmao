// SPDX-License-Identifier: MIT
// A compile request, as a GPU client hands it to the system's shader compiler (drivers/TegraGM20BMetal/compiler) through the
// kernel: a render state's descriptor fields, or a kernel's limits, stage input and dispatch, each stage's function and the
// library it comes from, and the variant. It carries the key the client made, which the compiler checks against the key it
// makes itself from the rest (TegraGM20BMetalCompileRequest.mm), and no record. The record is filed under the request's
// content (store_key.h), so a client finds a record any process made from the same functions.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "direct_abi.h"
#include "gm20b_cache.h"
#include "pipeline_fields.h"
#include "render_variant.h"

namespace gm20b {

constexpr uint64_t kRequestLimit = uint64_t(256) << 20;  // TegraGM20BCompilerABI.h TegraGM20BCompileRequestLimit
// A state's stages and libraries are bounded by the request's bytes alone: these only keep a decode's counts sane, above any
// state Metal makes (its linked functions are unbounded). Bindings are Metal's own per-stage limit.
constexpr uint32_t kRequestStageLimit = 1u << 16, kRequestLibraryLimit = 1u << 16, kRequestBindLimit = 31;

using Digest = std::array<uint8_t, 32>;

// Facts: a kernel's compute state's answers to Metal's per-state queries (pack.h PackComputeFacts), which its creation
// needs before any dispatch; its one stage and limits as a kernel's, no dispatch. Library: a library the system's Metal
// front end makes at run time from a code generation request (codegen_request.h), with no stage or library of its own.
// Reflection: a render state's (the stages and fields of Render) or a compute state's (Kernel's one stage and limits)
// reflection blob (reflection_blob.h), one function's attributes (one stage tagged "a", function_attributes.h) or
// its buffer encoders (one stage tagged "b", buffer_encoders.h), which the compiler makes with no compile context and
// stores
enum class RequestKind : uint8_t {
    Render,
    Kernel,
    Facts,
    Library,
    Reflection
};

// MTLCodeGenServiceBuildRequest's request types a Library request carries (ImageFilterLink.h's is the filter link's;
// kCodeGenSpecialize is MTLSpecializeFunctionToArchive)
constexpr uint32_t kCodeGenImageFilters = 5, kCodeGenSource = 13, kCodeGenStitch = 14, kCodeGenSpecialize = 15;

// Primary: the state's record; Full: the same with its fragment program's split and its program variants, as a pack
// build makes them, which a client asks at the background class once its primary record is placed (so no creation waits
// for the extra compiles) and the compiler files under the primary's content too; Barrier: a storage form's barrier twin
// (Direct::make's PackForm::Barrier), asked the same way once its primary record is a storage form, as the pack gives
// each of its storage forms one; BarrierFull: the twin's full form, as Full is the state's. The values are on the wire.
enum class RequestForm : uint8_t {
    Primary,
    Full,
    Barrier,
    BarrierFull
};

inline bool fullForm(RequestForm f)
{
    return f == RequestForm::Full || f == RequestForm::BarrierFull;
}

// The form a full form's record is filed under besides its own: the record it adds to
inline RequestForm withoutFull(RequestForm f)
{
    return f == RequestForm::Full ? RequestForm::Primary : f == RequestForm::BarrierFull ? RequestForm::Barrier : f;
}

struct RequestLibrary {
    std::string path;        // where the client loaded it from ("": made from data)
    bool system = false;     // systemLibraryPath(path): the compiler reads it there, and `bytes` is empty
    std::string bytes;       // inline bytes when !system && libraryId == 0
    uint64_t libraryId = 0;  // connection-scoped CoW handle (TegraGM20BMethodCompilerRegisterLibrary)
};

// A stage's function: for a render state the vertex function, the fragment function, then the linked ones (tags "vl",
// "vp", "fl", "fp" as TegraGM20BMetalLibrary.mm's linkedKey lists them); for a kernel its function (tag "k"), then its linked
// ones (tags "kl", "kp", "kb", "kg": compute_key.h computeLinkedText)
struct RequestStage {
    std::string tag;
    std::string function;      // the library function it was made from
    std::string constants;     // its constants' text (TegraGM20BMetalPipelineKey.h constantsText)
    std::string name;          // the name it was made under (its specialized name, else `function`)
    bool specialized = false;  // made with constant values (newFunctionWithName:constantValues:, even with none)
    uint32_t library = 0;
    std::string group;  // "kg" only: the call-site group's name
};

// A kernel at one dispatch shape and its bindings (TegraGM20BMetalDirect.h KernelDispatch), and its descriptor's stage input
struct KernelRequest {
    uint32_t maxThreads = 0;
    bool multipleOfWidth = false, regionMode = false;
    uint32_t threads[3] = {};
    uint32_t memory[kRequestBindLimit] = {};
    std::vector<NmbDirectBoundTexture> textures;
    std::vector<NmbDirectBoundSampler> samplers;
    StageInFields stageIn;

    NmbDirectDispatch dispatch() const;
};

struct CompileRequest {
    uint64_t stack = 0;
    RequestKind kind = RequestKind::Render;
    RequestForm form = RequestForm::Primary;
    PipelineFields fields;
    std::vector<RequestStage> stages;
    std::vector<RequestLibrary> libraries;
    RenderVariant variant;
    KernelRequest kernel;
    std::string key;  // the CacheKey text the client made (a kernel's starts "kernel | ")
    uint32_t codeGenType = 0;
    std::string codeGen;  // a Library's code generation request, sent only for one
};

// A render request's fragment stage (tag "f", second where it has one), kNoStage for a state of no fragment function; and
// its first linked stage
constexpr size_t kNoStage = SIZE_MAX;
size_t fragmentStage(const CompileRequest &r);
size_t firstLinked(const CompileRequest &r);

std::string encodeRequest(const CompileRequest &r);
// `f` as the key names it (keyFieldWords): a colour or attribute of no format zeroed, the tessellator's defaults without
// a patch, an amplification below 1 as 1, rounding mode 1 as 0 (both round toward zero), booleans 0 or 1. The compiler
// applies these, so nothing the store name does not fold reaches code generation. False: a stride no descriptor holds
// (2^32 - 1 or more, other than MTLBufferLayoutStrideDynamic, which the key names as 2^32 - 1).
bool canonicalize(PipelineFields &f);
// False: not a request this build reads (a bound passed, a stage naming no library, a system library given bytes)
bool decodeRequest(const std::string &bytes, CompileRequest &out);

Digest sha256(const void *bytes, size_t size);

// The CacheKey of a kernel's compute key (TegraGM20BMetalComplete.mm kernel())
CacheKey kernelCacheKey(uint64_t stack, const std::string &computeKey);
// Whether `r` asks a compute state's reflection (a Reflection of Kernel's one stage), and that reflection's CacheKey of the
// state's computeStateKey (compute_key.h)
bool computeReflection(const CompileRequest &r);
CacheKey computeReflectionKey(uint64_t stack, const std::string &stateKey);
// Whether `r` asks one function's attributes, and their CacheKey of its functionAttributesKey (function_attributes.h)
bool functionAttributes(const CompileRequest &r);
CacheKey functionAttributesCacheKey(uint64_t stack, const std::string &functionKey);
// Whether `r` asks one function's buffer encoders, and their CacheKey of its bufferEncodersKey (buffer_encoders.h)
bool bufferEncoders(const CompileRequest &r);
CacheKey bufferEncodersCacheKey(uint64_t stack, const std::string &functionKey);
// The CacheKey of a compute state's facts, of its compute_key.h computeStateKey
CacheKey factsCacheKey(uint64_t stack, const std::string &stateKey);
// The CacheKey of the library the front end makes of code generation request `bytes` of `type`: the request's SHA-256
CacheKey libraryCacheKey(uint64_t stack, uint32_t type, const std::string &bytes);

}  // namespace gm20b
