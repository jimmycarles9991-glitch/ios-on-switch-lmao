// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalInternal.h"
#import "TegraGM20BMetalBinaryArchive.h"
#import "TegraGM20BMetalComputeState.h"
#import "TegraGM20BMetalReflection.h"
#import "TegraGM20BMetalStatesKey.h"
#include "TegraGM20BMetalComplete.h"
#include "TegraGM20BMetalCompileRequest.h"
#include "compute_key.h"
#include "metallib.h"
#include "pack.h"

@implementation SwitchMetalComputeState

- (BOOL)conformsToProtocol:(Protocol *)protocol
{
    return protocol == @protocol(MTLComputePipelineState) || [super conformsToProtocol:protocol];
}

- (NSUInteger)maxTotalThreadsPerThreadgroup
{
    return _facts.maxThreads;
}

- (NSUInteger)threadExecutionWidth
{
    return _facts.executionWidth;
}

- (NSUInteger)staticThreadgroupMemoryLength
{
    return _facts.staticMemory;
}

// The descriptor's, as Metal's state answers it
- (BOOL)supportIndirectCommandBuffers
{
    return _direct.supportIndirectCommandBuffers;
}

@end

// The function's AIR hash and its library function's name, as the facade's library read them
static bool airOf(id<MTLFunction> function, gm20b::AirHash &out, NSString *__strong &name)
{
    if (![(id)function isKindOfClass:[SwitchMetalFunction class]] || !((SwitchMetalFunction *)function)->_air)
        return false;
    SwitchMetalFunction *f = (SwitchMetalFunction *)function;
    out = *f->_air;
    name = @(libraryName(f).c_str());
    return true;
}

// The descriptor's linked functions as the compute key takes them (compute_key.h computeLinkedText); false for any the
// key cannot name: another device's
static bool linkedFunctions(MTLComputePipelineDescriptor *d, std::vector<gm20b::LinkedComputeFunction> &linked)
{
    MTLLinkedFunctions *l = d.linkedFunctions;
    const auto add = [&](id function, uint32_t list, std::optional<std::string> group = std::nullopt) {
        gm20b::LinkedComputeFunction f;
        f.hidden = list == 1;
        f.binary = list == 2;
        f.group = std::move(group);
        NSString *libraryName = nil;
        if (![function isKindOfClass:[SwitchMetalFunction class]] || !airOf(function, f.air, libraryName))
            return false;
        f.constants = ((SwitchMetalFunction *)function)->_constants;
        f.symbol = libraryName.UTF8String ?: "";
        f.name = [function name].UTF8String ?: "";
        linked.push_back(std::move(f));
        return true;
    };
    for (const uint32_t list : {0u, 1u, 2u})
        for (id function in list == 2 ? l.binaryFunctions : list == 1 ? l.privateFunctions : l.functions)
            if (!add(function, list))
                return false;
    for (NSString *group in [l.groups.allKeys sortedArrayUsingSelector:@selector(compare:)])
        for (id function in l.groups[group])
            if (!add(function, 3,
                     std::string(group.UTF8String ?: "", [group lengthOfBytesUsingEncoding:NSUTF8StringEncoding])))
                return false;
    return true;
}

// The creations under MTLPipelineOptionFailOnBinaryArchiveMiss lists (this device's, TegraGM20BMetalBinaryArchive.mm), as
// Metal's answer: false (and Metal's error) where none holds the state and the pack holds no kernel of it, never compiled.
// An archive names a state by its function and `constants` alone, never its linked functions.
static bool archivesAnswer(SwitchMetalComputeState *s, SwitchMetalFunction *f, const std::string &constants,
                           MTLPipelineOption &options, NSError **error)
{
    NSString *name = s->_libraryName ?: f.name;
    const std::string key =
        gm20b::computeArchiveKey(name.UTF8String ?: "", constants, s->_maxThreads, s->_multipleOfWidth);
    const gm20b::ArchiveAnswer answer =
        gm20bArchiveAnswer(s->_direct.binaryArchives, options, gm20b::Pack::fnv(key.data(), key.size()));
    options &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    s->_direct.binaryArchives = nil;
    if (answer != gm20b::ArchiveAnswer::Missed ||
        gm20bPackHoldsKernels(gm20b::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth)))
        return true;
    const uint64_t keyHash = gm20b::Pack::fnv(key.data(), key.size());
    if (gm20bFirstNaming(keyHash, 1))
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: no compute state %{public}s (%{public}s): the archives it lists miss "
                     "%016llx under FailOnBinaryArchiveMiss",
                     getprogname(), f.name.UTF8String ?: "-", s->_direct.label.UTF8String ?: "-",
                     (unsigned long long)keyHash);
    if (error)
        *error = gm20bArchiveMissError(f.name, "compute");
    return false;
}

// The answers of a keyed state the pack has no facts of, the shader compiler's (TegraGM20BMetalComplete.h facts); a stage input is keyed
// after the constants and sent beside them (KernelRequest::stageIn)
static bool compilerAnswers(SwitchMetalComputeState *s, SwitchMetalFunction *f, NSError **error)
{
    std::string failure = "no shader compiler comes this boot";
    gm20b::PackComputeFacts made = {};
    const std::string key = gm20b::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth);
    const uint64_t keyHash = gm20b::Pack::fnv(key.data(), key.size());
#if SWITCH_METAL_STATS
    gm20bNoteMiss("compute", keyHash, key, s->_direct.label);
#endif
    if (!gRM->completion || !gRM->completion->facts(f, s->_direct, key, made, failure)) {
        if (gm20bFirstNaming(keyHash, 2))
            os_log_error(gm20bLog(),
                         "SwitchMetal: %{public}s: no compute state %{public}s (%{public}s) %016llx: %{public}s",
                         getprogname(), f.name.UTF8String ?: "-", s->_direct.label.UTF8String ?: "-",
                         (unsigned long long)keyHash, failure.c_str());
        if (error)
            *error = gm20bCreationError(failure);
        return false;
    }
    s->_facts = made;
    if (error)
        *error = nil;
    return true;
}

id gm20bComputeState(id device, MTLComputePipelineDescriptor *descriptor, MTLPipelineOption options,
                     MTLAutoreleasedComputePipelineReflection *reflection, NSError **error)
{
    descriptor = [descriptor copy];
    for (id<MTLFunction> binary in descriptor.linkedFunctions.binaryFunctions)
        if (![(id)binary isKindOfClass:[SwitchMetalFunction class]] ||
            !(binary.options & MTLFunctionOptionCompileToBinary)) {
            if (error)
                *error = gm20bCreationError(
                    "a binary function must be this device's visible function made with CompileToBinary");
            return nil;
        }
    id<MTLFunction> function = descriptor.computeFunction;
    SwitchMetalComputeState *s = [SwitchMetalComputeState new];
    s->_device = device;
    SwitchMetalFunction *f =
        [(id)function isKindOfClass:[SwitchMetalFunction class]] ? (SwitchMetalFunction *)function : nil;
    NSString *libraryName = nil;
    std::vector<gm20b::LinkedComputeFunction> linked;
    // a function of another device (Metal's validation error), or one whose library lists no AIR hash of it, which no
    // compile request can name
    if (!f || !linkedFunctions(descriptor, linked) || !airOf(function, s->_air, libraryName)) {
        constexpr const char *unkeyed =
            "a compute or linked function of another device, or one whose library lists no AIR hash of it";
        os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no compute state: %{public}s (%{public}s)", getprogname(),
                     unkeyed, descriptor.label.UTF8String ?: "-");
        if (error)
            *error = gm20bCreationError(unkeyed);
        return nil;
    }
    s->_libraryName = libraryName;
    const std::string stageIn = gm20b::stageInText(gm20b::stageInFieldsOf(descriptor.stageInputDescriptor));
    s->_stageIn = !stageIn.empty();
    s->_constants = gm20b::computeStateConstants(f->_constants, stageIn, linked);
    s->_maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
    s->_multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
    s->_direct = descriptor;
    s->_function = f;
    if (!archivesAnswer(s, f, f->_constants + stageIn, options, error))
        return nil;
    if (const gm20b::PackComputeFacts *facts = gm20bPackComputeFacts(
            gm20b::computeStateKey(s->_air, s->_constants, s->_maxThreads, s->_multipleOfWidth))) {
        s->_facts = *facts;
        if (error)
            *error = nil;
    } else if (!compilerAnswers(s, f, error)) {
        return nil;
    }
    if (reflection && !gm20bComputeReflection(s, options, reflection, error))
        return nil;
    s->_threadLimit = uint32_t(std::min<NSUInteger>(1024, s.maxTotalThreadsPerThreadgroup));
    if (s->_maxThreads)
        s->_threadLimit = std::min(s->_threadLimit, s->_maxThreads);
    s.label = descriptor.label;
    return s;
}
