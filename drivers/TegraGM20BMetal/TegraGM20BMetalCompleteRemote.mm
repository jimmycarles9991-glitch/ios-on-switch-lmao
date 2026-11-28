// SPDX-License-Identifier: MIT
// Completion's side of the system shader compiler (remote.h): a state's or kernel's request made of the facade's own
// functions and libraries, its record looked up in the compiler's store, and asked of the compiler when the store lacks it.
#import "TegraGM20BMetalComplete.h"

#include <os/log.h>
#include <pthread/qos.h>
#include "TegraGM20BMetalCompileRequest.h"
#include "TegraGM20BMetalInternal.h"
#include "compute_facts_entry.h"
#include "compute_record.h"
#include "reflection_blob.h"
#include "library_entry.h"
#include "function_tags.h"
#include "remote_ask.h"

namespace gm20b {
namespace {

// What a request's stages are beside it: each stage's function as the store key names it, and each library's source by
// its index in the request
struct RequestSources {
    std::vector<StageContent> stages;
    std::vector<LibrarySource *> libraries;
    std::map<const LibrarySource *, uint32_t> seen;
};

#if SWITCH_METAL_STATS
const char *classText(uint32_t qos)
{
    return qos == QOS_CLASS_USER_INTERACTIVE ? "UI"
           : qos == QOS_CLASS_USER_INITIATED ? "IN"
           : qos == QOS_CLASS_DEFAULT        ? "DF"
           : qos == QOS_CLASS_UTILITY        ? "UT"
           : qos == QOS_CLASS_BACKGROUND     ? "BG"
                                             : "fixed";
}

// A kernel or compute-state facts the shader compiler answered at run time, whole, for tools/gpu/kernelharvest.py:
// the function its library names, that library (a system library's path, else "data": the tool finds it by the AIR the
// key names), " spec"
// where the function was made with constant values, and its cache key, in parts of 700 characters
void logComputeHarvest(const char *kind, const CacheKey &k, const CompileRequest &r)
{
    const RequestStage &s = r.stages[0];
    std::string lib = r.libraries[s.library].path;
    if (!systemLibraryPath(lib))
        lib = "data";
    const std::string text = "lib=" + lib + " function=" + s.function + (s.specialized ? " spec " : " ") + k.key;
    constexpr size_t kPart = 700;
    const size_t parts = (text.size() + kPart - 1) / kPart;
    for (size_t i = 0; i < parts; i++)
        os_log(gm20bLog(), "SwitchMetal: %{public}s %016llx %zu/%zu %{public}s", kind, (unsigned long long)k.name(),
               i + 1, parts, text.substr(i * kPart, kPart).c_str());
}
#endif

// A function of this facade as a request's stage, its library added to `r` once (`sources` beside it); false: one no
// request can name (another device's, or one whose library lists no AIR for it). The bytes are the AIR slice the facade
// read; `every` sends a system library's too, as a data library. A library sent to the compiler is registered by its
// source's id (remote.h registerLibrary), with no digest.
bool addStage(CompileRequest &r, RequestSources &sources, const char *tag, id function, bool bytes, bool every = false,
              Remote *remote = nullptr)
{
    if (![function isKindOfClass:[SwitchMetalFunction class]])
        return false;
    SwitchMetalFunction *f = function;
    const void *data = f->_source ? f->_source->bytes : nullptr;
    size_t length = f->_source ? f->_source->size : 0;
    if (!data || !f->_air)
        return false;
    const auto [at, first] = sources.seen.try_emplace(f->_source.get(), uint32_t(r.libraries.size()));
    if (first) {
        if (r.libraries.size() >= kRequestLibraryLimit)
            return false;
        RequestLibrary library;
        library.path = f->_source->path;
        library.system = f->_source->system && !every;
        if ((bytes || every) && !library.system) {
            const uint64_t handle = remote ? remote->registerLibrary(data, length, f->_source->id) : 0;
            if (handle) {
                library.libraryId = handle;
            } else {
                library.bytes.assign(static_cast<const char *>(data), length);
            }
        }
        sources.libraries.push_back(f->_source.get());
        r.libraries.push_back(std::move(library));
    }
    std::call_once(f->_factsOnce, [f] {
        f->_facts = listedFacts(f->_source->bytes, f->_source->size, f->_libraryName);
    });
    sources.stages.push_back({*f->_air, f->_facts});
    RequestStage s;
    s.tag = tag;
    s.function = libraryName(f);
    s.constants = f->_constants;
    s.name = f.name.UTF8String ?: "";
    s.specialized = f->_specialized;
    s.library = at->second;
    r.stages.push_back(std::move(s));
    return true;
}

// A compute state's kernel and its linked functions as its request's stages: "k", then "kl" for each visible and "kp" for
// each private one, then "kb" for binary functions and "kg" for groups by name, in the order its key names them
// (compute_key.h computeLinkedText)
bool addKernelStages(CompileRequest &r, RequestSources &sources, id kernel, MTLComputePipelineDescriptor *d, bool bytes,
                     bool every, Remote *remote = nullptr)
{
    if (!addStage(r, sources, "k", kernel, bytes, every, remote))
        return false;
    for (id f in d.linkedFunctions.functions)
        if (!addStage(r, sources, "kl", f, bytes, every, remote))
            return false;
    for (id f in d.linkedFunctions.privateFunctions)
        if (!addStage(r, sources, "kp", f, bytes, every, remote))
            return false;
    for (id f in d.linkedFunctions.binaryFunctions)
        if (!addStage(r, sources, "kb", f, bytes, every, remote))
            return false;
    for (NSString *group in [d.linkedFunctions.groups.allKeys sortedArrayUsingSelector:@selector(compare:)])
        for (id f in d.linkedFunctions.groups[group]) {
            if (!addStage(r, sources, "kg", f, bytes, every, remote))
                return false;
            r.stages.back().group.assign(group.UTF8String ?: "",
                                         [group lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        }
    return r.stages.size() <= kRequestStageLimit;
}

#if SWITCH_METAL_DIAGNOSTICS
thread_local Obtained tObtained;
#endif

}  // namespace

#if SWITCH_METAL_DIAGNOSTICS
Obtained Completion::lastObtained()
{
    return tObtained;
}

void Completion::noteObtained(const Obtained &o)
{
    tObtained = o;
}
#endif

bool Completion::renderRequest(const Ask &a, const CacheKey &k, bool bytes, CompileRequest &r, StoreKey &store,
                               bool every, RequestKind kind, RequestForm form)
{
    MTLRenderPipelineDescriptor *d = a.facade;
    if (!d)
        return false;
    r = {};
    r.stack = stack_;
    r.kind = kind;
    r.form = form;
    r.key = k.key;
    r.fields = fieldsOf(d);
    r.variant = a.variant;
    RequestSources sources;
    @autoreleasepool {
        if (!addStage(r, sources, "v", d.vertexFunction, bytes, every, remote_.get()) ||
            (d.fragmentFunction && !addStage(r, sources, "f", d.fragmentFunction, bytes, every, remote_.get())))
            return false;
        const std::pair<const char *, NSArray *> linked[] = {{"vl", d.vertexLinkedFunctions.functions},
                                                             {"vp", d.vertexLinkedFunctions.privateFunctions},
                                                             {"fl", d.fragmentLinkedFunctions.functions},
                                                             {"fp", d.fragmentLinkedFunctions.privateFunctions}};
        for (const auto &[tag, functions] : linked)
            for (id f in functions)
                if (!addStage(r, sources, tag, f, bytes, every, remote_.get()))
                    return false;
    }
    if (r.stages.size() > kRequestStageLimit)
        return false;
    store = storeKeyOf(k, r, sources.stages, remote_->scope());
    return true;
}

bool Completion::storedEntry(const StoreKey &store, const std::function<bool(std::string_view bytes)> &decoded)
{
    std::string_view bytes;
    for (uint32_t k = 0; k < kStoreCandidates; ++k)
        if (remote_->find(candidateName(store.name, k), bytes) && decoded(bytes))
            return true;
    return false;
}

RecordCache::Found Completion::fromStore(const StoreKey &store, const std::string &packKey, RuntimeRecord &record,
                                         std::string &why)
{
    RecordCache::Found found = RecordCache::Found::None;
    storedEntry(store, [&](std::string_view bytes) {
        found = decodeEntry(bytes, store.content, record, why);
        return found != RecordCache::Found::None;
    });
    if (found == RecordCache::Found::Record)
        rekeyRecord(record, packKey);
    return found;
}

std::string Completion::packKeyOf(const CompileRequest &r)
{
    bool own = false;
    const std::string key = requestPackKey(r, own);
    const std::string &base = own ? r.key : key;
    return withoutFull(r.form) == RequestForm::Barrier ? base + " | barrier form" : base;
}

// The wait split where it went (obtained.h; -1 for a part not measured): the request queued until its compile
// started, the compile, the compiler's answer after it, this thread's wake after that, and the store lookup. The split
// stays inside the parentheses: tools/gpu/logharvest.py reads the name and key after "for a compile slot: ".
#if SWITCH_METAL_STATS
void Completion::logRemote(const Ask &a, const CacheKey &k, RecordCache::Found found, const char *how,
                           const Obtained &o)
{
    const bool made = found == RecordCache::Found::Record;
    os_log(gm20bLog(),
           "SwitchMetal: a state the pack lacks %{public}s in %llu ms (%{public}s, %{public}s; queue %lld us, compile "
           "%lld us, "
           "answer %lld us at priority %u, wake %lld us, store %lld us) after 0 ms for a compile slot: %016llx "
           "%{public}.400s",
           made ? "made" : "refused", (unsigned long long)(o.waitUs / 1000), classText(qos_class_self()), how,
           (long long)o.queueUs, (long long)o.compileUs, (long long)o.answerUs, o.answerPriority, (long long)o.wakeUs,
           (long long)o.storeUs, (unsigned long long)k.name(), k.key.c_str());
    logHarvestParts(k.name(), a.descriptor.label ?: a.facade.label, k.key);
}
#endif

bool Completion::remoteNow(const Ask &a, const CacheKey &k, Compiler::State &state, RuntimeRecord &record,
                           std::string &failure)
{
    CompileRequest r;
    StoreKey store;
    if (!renderRequest(a, k, false, r, store))
        return failure = kInexpressible, false;
    const std::string packKey = packKeyOf(r);
    std::string why;
    RecordCache::Found found = fromStore(store, packKey, record, why);
#if SWITCH_METAL_DIAGNOSTICS
    const uint64_t began = nowUs();
    tObtained = {"store", 0, uint32_t(qos_class_self()), 0};
#endif
    if (found == RecordCache::Found::None) {
        if (!remote_->taking())
            return failure = kNoCompilerComes, false;
        Remote::Answered answered;
        Remote::Answer answer = Remote::Answer::Unavailable;
#if SWITCH_METAL_DIAGNOSTICS
        uint64_t sent = began;
#endif
        for (const bool again : {false, true}) {
            if (!renderRequest(a, k, true, r, store, again))
                return failure = kInexpressible, false;
            const std::string request = encodeRequest(r);
            SWITCH_METAL_DIAGNOSTIC(sent = nowUs());
            answer = remote_->compile(request, &answered);
            if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                found = fromStore(store, packKey, record, why);
            if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
                break;
        }
#if SWITCH_METAL_DIAGNOSTICS
        static const char *const kinds[] = {"compiled", "joined", "stored"};
        const uint32_t how = answered.how;
        tObtained.how = (how & 0xff) < 3 ? kinds[how & 0xff] : "?";
        tObtained.ran = how;
        const uint64_t foundUs = nowUs();
        tObtained.waitUs = foundUs - began;
        const TegraGM20BAbiCompileTimes &t = answered.times;
        tObtained.split(sent, t.compile_start, t.compile_end, t.answered, answered.at, foundUs);
        tObtained.answerPriority = t.answer_priority;
#endif
        switch (askFailure(answer, found != RecordCache::Found::None, remote_->declined())) {
        case AskFailure::None:
            break;
        case AskFailure::CouldNotRun:
            state = Compiler::State::Failed;
            return true;
        case AskFailure::NoCompilerComes:
            return failure = kNoCompilerComes, false;
        case AskFailure::NotTaken:
            return failure = kNotTaken, false;
        case AskFailure::NotStored:
            return failure = kNotStored, false;
        }
        SWITCH_METAL_STAT(logRemote(a, k, found, "by the shader compiler", tObtained));
    } else {
        SWITCH_METAL_STAT(logRemote(a, k, found, "from the shader compiler's store", tObtained));
    }
    compiler_->keep(k, found, record, why);
    state = found == RecordCache::Found::Record ? Compiler::State::Ready : Compiler::State::Refused;
    if (state == Compiler::State::Refused)
        failure = why;
    return true;
}

bool Completion::remoteLater(const Ask &a, const CacheKey &k,
                             std::function<void(std::shared_ptr<const RuntimeRecord>)> done, bool again)
{
    CompileRequest r;
    StoreKey store;
    if (!renderRequest(a, k, false, r, store, again)) {
        done(nullptr);
        return true;
    }
    const std::string packKey = packKeyOf(r);
    const auto settle = [this, k, done](RecordCache::Found found, const RuntimeRecord &record, const std::string &why) {
        compiler_->keep(k, found, record, why);
        if (found == RecordCache::Found::Record) {
            done(std::make_shared<const RuntimeRecord>(record));
            return;
        }
        std::shared_ptr<const RuntimeRecord> kept;
        const Compiler::State state = compiler_->ask(
            k,
            [done](std::shared_ptr<const RuntimeRecord> made, const std::string &) {
                done(std::move(made));
            },
            &kept);
        if (state == Compiler::State::Ready || state == Compiler::State::Refused)
            done(state == Compiler::State::Ready ? std::move(kept) : nullptr);
    };
    RuntimeRecord record;
    std::string why;
    if (const RecordCache::Found found = fromStore(store, packKey, record, why); found != RecordCache::Found::None) {
        SWITCH_METAL_STAT(logRemote(a, k, found, "from the shader compiler's store",
                                    Obtained{"store", 0, uint32_t(qos_class_self())}));
        settle(found, record, why);
        return true;
    }
    if (!remote_->taking() || !renderRequest(a, k, true, r, store, again)) {
        done(nullptr);
        return true;
    }
    const std::string request = encodeRequest(r);
#if SWITCH_METAL_STATS
    const uint64_t began = nowUs();
#else
    const uint64_t began = 0;
#endif
    const Ask asked = a;
    uint64_t cookie = 0;
    const bool sent = remote_->compileAsync(
        request, a.waited, a.qos,
        [this, asked, k, packKey, store, began, settle, done, again](Remote::Answer answer,
                                                                     const Remote::Answered &answered) {
            RuntimeRecord made;
            std::string refusal;
            const RecordCache::Found found = answer == Remote::Answer::Made || answer == Remote::Answer::Refused
                                                 ? fromStore(store, packKey, made, refusal)
                                                 : RecordCache::Found::None;
            switch (askNext(answer, found != RecordCache::Found::None, again, remote_->declined())) {
            case AskNext::Use: {
#if SWITCH_METAL_STATS
                Obtained o{"compiled", nowUs() - began, asked.qos, answered.how};
                const TegraGM20BAbiCompileTimes &t = answered.times;
                o.split(began, t.compile_start, t.compile_end, t.answered, answered.at, began + o.waitUs);
                o.answerPriority = t.answer_priority;
                logRemote(asked, k, found, "by the shader compiler", o);
#else
                (void)began;
                (void)answered;
#endif
                return settle(found, made, refusal);
            }
            case AskNext::AskAgain:
                return (void)remoteLater(asked, k, done, true);
            case AskNext::End:
                return done(nullptr);
            }
        },
        &cookie);
    if (sent && a.bindWait && cookie) {
        std::weak_ptr<Remote> weak = remote_;
        a.bindWait([weak = std::move(weak), cookie] {
            const auto remote = weak.lock();
            return remote && remote->waitAsync(cookie);
        });
    }
    if (!sent)
        done(nullptr);
    return true;
}

Compiler::Outcome Completion::remoteKernel(id facadeFunction, MTLComputePipelineDescriptor *descriptor,
                                           const NmbDirectDispatch &dispatch, const CacheKey &k, ComputeRecord &record,
                                           std::string &why, bool &taken)
{
    taken = false;
    CompileRequest r;
    StoreKey store;
    RequestSources sources;
    const auto request = [&](bool bytes, bool every) {
        r = {};
        sources = {};
        r.stack = stack_;
        r.kind = RequestKind::Kernel;
        r.key = k.key;
        KernelRequest &q = r.kernel;
        q.maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
        q.multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
        q.regionMode = dispatch.regionMode;
        memcpy(q.threads, dispatch.threads, sizeof q.threads);
        memcpy(q.memory, dispatch.memory, sizeof q.memory);
        q.textures.assign(dispatch.textures, dispatch.textures + dispatch.textureCount);
        q.samplers.assign(dispatch.samplers, dispatch.samplers + dispatch.samplerCount);
        q.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);
        if (q.textures.size() > kRequestBindLimit || q.samplers.size() > kRequestBindLimit ||
            !addKernelStages(r, sources, facadeFunction, descriptor, bytes, every, remote_.get()))
            return false;
        store = storeKeyOf(k, r, sources.stages, remote_->scope());
        return true;
    };
    const auto stored = [&]() {
        RecordCache::Found found = RecordCache::Found::None;
        storedEntry(store, [&](std::string_view bytes) {
            found = decodeKernelEntry(bytes, store.content, record, why);
            return found != RecordCache::Found::None;
        });
        return found;
    };
    if (!request(false, false))
        return Compiler::Outcome::Failed;
    RecordCache::Found found = stored();
    AskFailure failure = AskFailure::None;
    for (const bool again : {false, true}) {
        if (found != RecordCache::Found::None || !remote_->taking() || !request(true, again))
            break;
        const Remote::Answer answer = remote_->compile(encodeRequest(r));
        if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
            found = stored();
        failure = askFailure(answer, found != RecordCache::Found::None, remote_->declined());
        if (failure == AskFailure::CouldNotRun) {
            taken = true;
            return Compiler::Outcome::Failed;
        }
        if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
            break;
    }
    if (found == RecordCache::Found::None) {
        why = failure == AskFailure::NotTaken          ? kNotTaken
              : failure == AskFailure::NotStored       ? kNotStored
              : failure == AskFailure::NoCompilerComes ? kNoCompilerComes
                                                       : "";
        return Compiler::Outcome::Failed;
    }
    taken = true;
#if SWITCH_METAL_STATS
    logComputeHarvest("kernel harvest", k, r);
#endif
    return found == RecordCache::Found::Record ? Compiler::Outcome::Made : Compiler::Outcome::Refused;
}

bool Completion::storedReflection(const std::function<bool(bool, bool, CompileRequest &, StoreKey &)> &request,
                                  std::string &blob, std::string &why)
{
    CompileRequest r;
    StoreKey store;
    std::string text;
    bool made = false;
    const auto stored = [&] {
        return storedEntry(store, [&](std::string_view bytes) {
            return decodeReflectionEntry(bytes, store.content, made, text);
        });
    };
    if (!request(false, false, r, store))
        return why = kInexpressible, false;
    bool found = stored();
    for (const bool again : {false, true}) {
        if (found || !remote_->taking() || !request(true, again, r, store))
            break;
        const Remote::Answer answer = remote_->compile(encodeRequest(r));
        if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
            found = stored();
        if (askNext(answer, found, again, remote_->declined()) != AskNext::AskAgain)
            break;
    }
    if (!found)
        return why = remote_->taking() ? kNotStored : kNoCompilerComes, false;
    if (!made)
        return why = text, false;
    blob = std::move(text);
    return true;
}

bool Completion::reflection(Ask a, std::string &blob, std::string &why)
{
    CacheKey k;
    if (!keyOf(a, k))
        return why = kInexpressible, false;
    return storedReflection(
        [&](bool bytes, bool every, CompileRequest &r, StoreKey &store) {
            return renderRequest(a, k, bytes, r, store, every, RequestKind::Reflection);
        },
        blob, why);
}

bool Completion::reflection(id function, MTLComputePipelineDescriptor *descriptor, const std::string &stateKey,
                            std::string &blob, std::string &why)
{
    const CacheKey k = computeReflectionKey(stack_, stateKey);
    return storedReflection(
        [&](bool bytes, bool every, CompileRequest &r, StoreKey &store) {
            r = {};
            r.stack = stack_;
            r.kind = RequestKind::Reflection;
            r.key = k.key;
            r.kernel.maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
            r.kernel.multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
            r.kernel.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);
            RequestSources sources;
            if (!addKernelStages(r, sources, function, descriptor, bytes, every, remote_.get()))
                return false;
            store = storeKeyOf(k, r, sources.stages, remote_->scope());
            return true;
        },
        blob, why);
}

bool Completion::functionReflection(const char *tag, id function, const CacheKey &k, std::string &blob,
                                    std::string &why)
{
    return storedReflection(
        [&](bool bytes, bool every, CompileRequest &r, StoreKey &store) {
            r = {};
            r.stack = stack_;
            r.kind = RequestKind::Reflection;
            r.key = k.key;
            RequestSources sources;
            if (!addStage(r, sources, tag, function, bytes, every, remote_.get()))
                return false;
            store = storeKeyOf(k, r, sources.stages, remote_->scope());
            return true;
        },
        blob, why);
}

bool Completion::attributes(id function, const std::string &functionKey, std::string &blob, std::string &why)
{
    return functionReflection("a", function, functionAttributesCacheKey(stack_, functionKey), blob, why);
}

bool Completion::bufferEncoders(id function, const std::string &functionKey, std::string &blob, std::string &why)
{
    return functionReflection("b", function, bufferEncodersCacheKey(stack_, functionKey), blob, why);
}

bool Completion::facts(id facadeFunction, MTLComputePipelineDescriptor *descriptor, const std::string &stateKey,
                       PackComputeFacts &out, std::string &failure)
{
    const CacheKey k = factsCacheKey(stack_, stateKey);
    CompileRequest r;
    StoreKey store;
    RequestSources sources;
    const auto request = [&](bool bytes, bool every) {
        r = {};
        r.stack = stack_;
        r.kind = RequestKind::Facts;
        r.key = k.key;
        r.kernel.maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
        r.kernel.multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
        r.kernel.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);
        sources = {};
        if (!addKernelStages(r, sources, facadeFunction, descriptor, bytes, every, remote_.get()))
            return false;
        store = storeKeyOf(k, r, sources.stages, remote_->scope());
        return true;
    };
    std::string refusal;
    const auto stored = [&]() {
        RecordCache::Found found = RecordCache::Found::None;
        storedEntry(store, [&](std::string_view bytes) {
            found = decodeFactsEntry(bytes, store.content, out, refusal);
            return found != RecordCache::Found::None;
        });
        return found;
    };
    if (!request(false, false))
        return failure = kInexpressible, false;
    RecordCache::Found found = stored();
    Remote::Answer answer = Remote::Answer::Unavailable;
#if SWITCH_METAL_STATS
    const bool fromStore = found != RecordCache::Found::None;
    const uint64_t began = nowUs();
#endif
    if (found == RecordCache::Found::None) {
        if (!remote_->serving() || !remote_->taking())
            return failure = kNoCompilerComes, false;
        for (const bool again : {false, true}) {
            if (!request(true, again))
                return failure = kInexpressible, false;
            answer = remote_->compile(encodeRequest(r));
            if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                found = stored();
            if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
                break;
        }
    }
    switch (askFailure(answer, found != RecordCache::Found::None, remote_->declined())) {
    case AskFailure::None:
        break;
    case AskFailure::CouldNotRun:
        return failure = "the kernel's compile could not run", false;
    case AskFailure::NoCompilerComes:
        return failure = kNoCompilerComes, false;
    case AskFailure::NotTaken:
        return failure = kNotTaken, false;
    case AskFailure::NotStored:
        return failure = kNotStored, false;
    }
    if (found == RecordCache::Found::Refusal)
        return failure = "the shader compiler made no compute state of the kernel: " + refusal, false;
#if SWITCH_METAL_STATS
    os_log(gm20bLog(),
           "SwitchMetal: a compute state the pack lacks answered %{public}s in %llu ms: max %u width %u memory %u: "
           "%{public}.300s",
           fromStore ? "from the shader compiler's store" : "by the shader compiler",
           (unsigned long long)((nowUs() - began) / 1000), out.maxThreads, out.executionWidth, out.staticMemory,
           stateKey.c_str());
    logComputeHarvest("facts harvest", k, r);
#endif
    return true;
}

bool Completion::library(uint32_t type, const std::string &bytes, MadeLibrary &out, std::string &failure, bool &refused)
{
    refused = false;
    CompileRequest r;
    r.stack = stack_;
    r.kind = RequestKind::Library;
    const CacheKey k = libraryCacheKey(stack_, type, bytes);
    r.key = k.key;
    r.codeGenType = type;
    r.codeGen = bytes;
    const StoreKey store = storeKeyOf(k, r, {}, remote_->scope());
    std::string log;
    const auto stored = [&]() {
        RecordCache::Found found = RecordCache::Found::None;
        storedEntry(store, [&](std::string_view entry) {
            found = decodeLibraryEntry(entry, store.content, out, log);
            return found != RecordCache::Found::None;
        });
        return found;
    };
    // a refusal is only read after this process's own ask: the front end makes a refused library again at each
    RecordCache::Found found =
        stored() == RecordCache::Found::Record ? RecordCache::Found::Record : RecordCache::Found::None;
    Remote::Answer answer = Remote::Answer::Unavailable;
    if (found == RecordCache::Found::None) {
        if (!remote_->serving() || !remote_->taking())
            return failure = kNoCompilerComes, false;
        const std::string request = encodeRequest(r);
        for (const bool again : {false, true}) {
            answer = remote_->compile(request);
            if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                found = stored();
            if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
                break;
        }
    }
    switch (askFailure(answer, found != RecordCache::Found::None, remote_->declined())) {
    case AskFailure::None:
        break;
    case AskFailure::CouldNotRun:
        return failure = "the system's Metal compiler could not run", false;
    case AskFailure::NoCompilerComes:
        return failure = kNoCompilerComes, false;
    case AskFailure::NotTaken:
        return failure = "the shader compiler did not take the library", false;
    case AskFailure::NotStored:
        return failure = "the shader compiler's store does not keep the library", false;
    }
    if (found == RecordCache::Found::Refusal) {
        refused = true;
        return failure = log, false;
    }
    return true;
}

}  // namespace gm20b
