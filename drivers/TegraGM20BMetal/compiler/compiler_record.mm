// SPDX-License-Identifier: MIT
// See compiler_record.h.
#import "compiler_record.h"

#include <cstring>
#include <vector>
#include "TegraGM20BCompilerABI.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawDirect.h"
#include "../raw/runtime/RawPipelineKey.h"
#include "../raw/runtime/RawRenderKey.h"
#include "../raw/runtime/raw_cache.h"
#include "../raw/runtime/raw_reflection_blob.h"
#include "../raw/runtime/RawOffline.h"
#include "compiler_request.h"

namespace switch_compiler {
namespace {

id<MTLFunction> function(id<MTLLibrary> library, const raw::RequestStage &s, std::string &why)
{
    MTLFunctionConstantValues *values = nil;
    if (s.specialized && !(values = raw::constantValuesOf(s.constants)))
        return why = "constants it cannot read: " + s.constants, nil;
    if (!s.specialized && !s.constants.empty())
        return why = "constants of a function made without them", nil;
    NSError *error = nil;
    NSString *name = @(s.function.c_str());
    id<MTLFunction> made = nil;
    if (s.name != s.function || s.tag == "kb") {
        MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
        d.name = name;
        d.specializedName = @(s.name.c_str());
        d.constantValues = values;
        d.options = s.tag == "kb" ? MTLFunctionOptionCompileToBinary : MTLFunctionOptionNone;
        made = [library newFunctionWithDescriptor:d error:&error];
    } else {
        made = values ? [library newFunctionWithName:name constantValues:values error:&error]
                      : [library newFunctionWithName:name];
    }
    if (!made)
        why = "no function " + s.function + (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
    else if (raw::constantsText(values) != s.constants)
        return why = "constants that do not read back as given", nil;
    return made;
}

int32_t resultOf(raw::CompileOutcome outcome)
{
    return outcome == raw::CompileOutcome::Made      ? TegraGM20BCompileMade
           : outcome == raw::CompileOutcome::Refused ? TegraGM20BCompileRefused
                                                     : TegraGM20BCompileFailed;
}

bool functions(const raw::CompileRequest &r, const Libraries &libraries, std::vector<id<MTLFunction>> &out,
               std::string &why)
{
    std::vector<id<MTLLibrary>> made;
    for (const raw::RequestLibrary &l : r.libraries) {
        id<MTLLibrary> library = libraries(l, why);
        if (!library)
            return false;
        made.push_back(library);
    }
    for (const raw::RequestStage &s : r.stages) {
        id<MTLFunction> f = function(made[s.library], s, why);
        if (!f)
            return false;
        out.push_back(f);
    }
    return true;
}

// `p`'s descriptor over the functions of `libraries`, checked to key as `p` was keyed
MTLRenderPipelineDescriptor *descriptor(const Prepared &p, const Libraries &libraries, uint64_t stack, std::string &why)
{
    const raw::CompileRequest &r = p.request;
    std::vector<id<MTLFunction>> made;
    if (!functions(r, libraries, made, why))
        return nil;
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    raw::applyFields(r.fields, d);
    d.vertexFunction = made[0];
    if (const size_t f = raw::fragmentStage(r); f != raw::kNoStage)
        d.fragmentFunction = made[f];
    if (uint64_t(made[0].patchType) != r.fields.patchType)
        return why = "a vertex function of another patch type than its request names", nil;
    NSMutableDictionary<NSString *, NSMutableArray *> *linked = [NSMutableDictionary dictionary];
    for (size_t i = raw::firstLinked(r); i < r.stages.size(); ++i) {
        NSString *tag = @(r.stages[i].tag.c_str());
        if (!linked[tag])
            linked[tag] = [NSMutableArray array];
        [linked[tag] addObject:made[i]];
    }
    d.vertexLinkedFunctions.functions = linked[@"vl"];
    d.vertexLinkedFunctions.privateFunctions = linked[@"vp"];
    d.fragmentLinkedFunctions.functions = linked[@"fl"];
    d.fragmentLinkedFunctions.privateFunctions = linked[@"fp"];
    bool own = false;
    raw::requestPackKey(r, own);
    raw::CacheKey runtime;
    if (!raw::renderCacheKey(d, p.packKey, own, stack, runtime) || runtime.key != p.key.key ||
        memcmp(runtime.vertexAir, p.key.vertexAir, sizeof runtime.vertexAir) ||
        memcmp(runtime.fragmentAir, p.key.fragmentAir, sizeof runtime.fragmentAir))
        return why = "functions m2v keys otherwise than their libraries", nil;
    return d;
}

int32_t render(const Prepared &p, const Libraries &libraries, raw::Direct &direct, uint64_t stack, std::string &entry,
               std::string &why)
{
    MTLRenderPipelineDescriptor *d = descriptor(p, libraries, stack, why);
    if (!d)
        return TegraGM20BCompileUnavailable;
    raw::RuntimeRecord record;
    const raw::CompileOutcome outcome = direct.make(d, p.packKey, record, why, raw::probeTexturesOf(p.request.variant));
    if (outcome != raw::CompileOutcome::Failed)
        entry = raw::encodeEntry(p.key, outcome == raw::CompileOutcome::Made ? &record : nullptr, why);
    return resultOf(outcome);
}

// A compute request's state over `made`, its stages' functions: the kernel, its linked functions ("kl" visible, "kp"
// private, "kb" binary, "kg" grouped), its stage input and limits
MTLComputePipelineDescriptor *computeDescriptor(const raw::CompileRequest &r, const std::vector<id<MTLFunction>> &made)
{
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = made[0];
    d.maxTotalThreadsPerThreadgroup = r.kernel.maxThreads;
    d.threadGroupSizeIsMultipleOfThreadExecutionWidth = r.kernel.multipleOfWidth;
    d.stageInputDescriptor = raw::stageInOf(r.kernel.stageIn);
    if (made.size() > 1) {
        NSMutableArray *visible = [NSMutableArray array], *hidden = [NSMutableArray array],
                       *binary = [NSMutableArray array];
        NSMutableDictionary<NSString *, NSMutableArray *> *groups = [NSMutableDictionary dictionary];
        for (size_t i = 1; i < made.size(); ++i) {
            const raw::RequestStage &stage = r.stages[i];
            if (stage.tag == "kg") {
                NSString *name = [[NSString alloc] initWithBytes:stage.group.data()
                                                          length:stage.group.size()
                                                        encoding:NSUTF8StringEncoding];
                if (!name)
                    return nil;
                NSMutableArray *members = groups[name];
                if (!members)
                    groups[name] = members = [NSMutableArray array];
                [members addObject:made[i]];
            } else {
                [(stage.tag == "kb" ? binary : stage.tag == "kp" ? hidden : visible) addObject:made[i]];
            }
        }
        MTLLinkedFunctions *linked = [MTLLinkedFunctions linkedFunctions];
        linked.functions = visible;
        linked.privateFunctions = hidden;
        linked.binaryFunctions = binary;
        linked.groups = groups;
        d.linkedFunctions = linked;
    }
    return d;
}

int32_t kernel(const Prepared &p, const Libraries &libraries, raw::Direct &direct, std::string &entry, std::string &why)
{
    std::vector<id<MTLFunction>> made;
    if (!functions(p.request, libraries, made, why))
        return TegraGM20BCompileUnavailable;
    MTLComputePipelineDescriptor *d = computeDescriptor(p.request, made);
    const NmbDirectDispatch dispatch = p.request.kernel.dispatch();
    raw::ComputeRecord record;
    const raw::CompileOutcome outcome = direct.kernel(d, dispatch, p.packKey, record, why);
    if (outcome != raw::CompileOutcome::Failed)
        entry = raw::encodeKernelEntry(p.key, outcome == raw::CompileOutcome::Made ? &record : nullptr, why);
    return resultOf(outcome);
}

int32_t reflection(const Prepared &p, const Libraries &libraries, uint64_t stack, std::string &entry, std::string &why)
{
    std::string blob;
    bool made = false;
    if (raw::computeReflection(p.request)) {
        std::vector<id<MTLFunction>> kernel;
        if (!functions(p.request, libraries, kernel, why))
            return TegraGM20BCompileUnavailable;
        made = offline::computeReflection(computeDescriptor(p.request, kernel), blob, why);
    } else {
        MTLRenderPipelineDescriptor *d = descriptor(p, libraries, stack, why);
        if (!d)
            return TegraGM20BCompileUnavailable;
        made = offline::renderReflection(d, blob, why);
    }
    entry = raw::encodeReflectionEntry(p.key, made ? &blob : nullptr, why);
    return made ? TegraGM20BCompileMade : TegraGM20BCompileRefused;
}

}  // namespace

int32_t makeReflectionEntry(const Prepared &p, const Libraries &libraries, uint64_t stack, std::string &entry,
                            std::string &why)
{
    @autoreleasepool {
        return reflection(p, libraries, stack, entry, why);
    }
}

id<MTLLibrary> offlineLibrary(const raw::RequestLibrary &l, const std::string &systemRoot, std::string &why)
{
    static RawOfflineDevice *const device = [RawOfflineDevice new];
    NSError *error = nil;
    id<MTLLibrary> made = nil;
    if (l.system) {
        std::string real;
        if (!SystemLibraries::resolve(systemRoot, l.path, real, why))
            return nil;
        made = [device newLibraryWithURL:[NSURL fileURLWithPath:@(real.c_str())] error:&error];
    } else {
        dispatch_data_t bytes =
            dispatch_data_create(l.bytes.data(), l.bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        made = [device newLibraryWithData:bytes error:&error];
    }
    if (!made)
        why = std::string("the translator refused the library ") + (l.path.empty() ? "of its data" : l.path) +
              (error ? std::string(": ") + error.localizedDescription.UTF8String : "");
    return made;
}

int32_t makeEntry(const Prepared &p, const Libraries &libraries, raw::Direct &direct, uint64_t stack,
                  std::string &entry, std::string &why)
{
    @autoreleasepool {
        return p.request.kind == raw::RequestKind::Kernel ? kernel(p, libraries, direct, entry, why)
                                                          : render(p, libraries, direct, stack, entry, why);
    }
}

}  // namespace switch_compiler
