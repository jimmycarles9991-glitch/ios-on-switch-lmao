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
#include "remote_ask.h"

namespace gm20b {
namespace {

#if SWITCH_METAL_DIAGNOSTICS
const char *classText(uint32_t qos)
{
    return qos == QOS_CLASS_USER_INTERACTIVE ? "UI"
           : qos == QOS_CLASS_USER_INITIATED ? "IN"
           : qos == QOS_CLASS_DEFAULT        ? "DF"
           : qos == QOS_CLASS_UTILITY        ? "UT"
           : qos == QOS_CLASS_BACKGROUND     ? "BG"
                                             : "fixed";
}

// A kernel the shader compiler answered at run time, whole, for tools/gpu/kernelharvest.py: the function its library names,
// that library (a system library's path, else data:<SHA-256 of its bytes>), " spec" where the function was made with
// constant values, and its cache key, in parts of 700 characters
void logKernelHarvest(const CacheKey &k, const CompileRequest &r, const std::vector<LibraryIdentity> &ids)
{
    const RequestStage &s = r.stages[0];
    std::string lib = r.libraries[s.library].path;
    if (!systemLibraryPath(lib)) {
        lib = "data:";
        char b[3];
        for (uint8_t x : ids[s.library].digest) {
            snprintf(b, sizeof b, "%02x", x);
            lib += b;
        }
    }
    const std::string text = "lib=" + lib + " function=" + s.function + (s.specialized ? " spec " : " ") + k.key;
    constexpr size_t kPart = 700;
    const size_t parts = (text.size() + kPart - 1) / kPart;
    for (size_t i = 0; i < parts; i++)
        os_log(gm20bLog(), "SwitchMetal: kernel harvest %016llx %zu/%zu %{public}s", (unsigned long long)k.name(),
               i + 1, parts, text.substr(i * kPart, kPart).c_str());
}
#endif

// A function of this facade as a request's stage, its library added to `r` once (`ids` beside it); false: one no request
// can name (another device's). The bytes are the AIR slice the facade read; `every` sends a system library's too, as a data
// library.
bool addStage(CompileRequest &r, std::vector<LibraryIdentity> &ids, std::map<const LibrarySource *, uint32_t> &seen,
              const char *tag, id function, bool bytes, bool every = false)
{
    if (![function isKindOfClass:[SwitchMetalFunction class]])
        return false;
    SwitchMetalFunction *f = function;
    const void *data = f->_source ? f->_source->bytes : nullptr;
    size_t length = f->_source ? f->_source->size : 0;
    if (!data)
        return false;
    const auto [at, first] = seen.try_emplace(f->_source.get(), uint32_t(r.libraries.size()));
    if (first) {
        if (r.libraries.size() >= kRequestLibraryLimit)
            return false;
        RequestLibrary library;
        library.path = f->_source->path;
        library.system = f->_source->system && !every;
        if ((bytes || every) && !library.system)
            library.bytes.assign(static_cast<const char *>(data), length);
        // sent as its bytes, the library is named by them, as the compiler names it (compiler_request.mm)
        ids.push_back(every ? identityOf(library) : f->_source->identity(data, length));
        r.libraries.push_back(std::move(library));
    }
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
bool addKernelStages(CompileRequest &r, std::vector<LibraryIdentity> &ids,
                     std::map<const LibrarySource *, uint32_t> &seen, id kernel, MTLComputePipelineDescriptor *d,
                     bool bytes, bool every)
{
    if (!addStage(r, ids, seen, "k", kernel, bytes, every))
        return false;
    for (id f in d.linkedFunctions.functions)
        if (!addStage(r, ids, seen, "kl", f, bytes, every))
            return false;
    for (id f in d.linkedFunctions.privateFunctions)
        if (!addStage(r, ids, seen, "kp", f, bytes, every))
            return false;
    for (id f in d.linkedFunctions.binaryFunctions)
        if (!addStage(r, ids, seen, "kb", f, bytes, every))
            return false;
    for (NSString *group in [d.linkedFunctions.groups.allKeys sortedArrayUsingSelector:@selector(compare:)])
        for (id f in d.linkedFunctions.groups[group]) {
            if (!addStage(r, ids, seen, "kg", f, bytes, every))
                return false;
            r.stages.back().group.assign(group.UTF8String ?: "",
                                         [group lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        }
    return r.stages.size() <= kRequestStageLimit;
}

thread_local Obtained tObtained;

}  // namespace

Obtained Completion::lastObtained()
{
    return tObtained;
}

void Completion::noteObtained(const Obtained &o)
{
    tObtained = o;
}

bool Completion::renderRequest(const Ask &a, const CacheKey &k, bool bytes, CompileRequest &r, Digest &name, bool every,
                               RequestKind kind)
{
    MTLRenderPipelineDescriptor *d = a.facade;
    if (!d)
        return false;
    r = {};
    r.stack = stack_;
    r.kind = kind;
    r.key = k.key;
    r.fields = fieldsOf(d);
    r.variant = a.variant;
    std::vector<LibraryIdentity> ids;
    std::map<const LibrarySource *, uint32_t> seen;
    @autoreleasepool {
        if (!addStage(r, ids, seen, "v", d.vertexFunction, bytes, every) ||
            (d.fragmentFunction && !addStage(r, ids, seen, "f", d.fragmentFunction, bytes, every)))
            return false;
        const std::pair<const char *, NSArray *> linked[] = {{"vl", d.vertexLinkedFunctions.functions},
                                                             {"vp", d.vertexLinkedFunctions.privateFunctions},
                                                             {"fl", d.fragmentLinkedFunctions.functions},
                                                             {"fp", d.fragmentLinkedFunctions.privateFunctions}};
        for (const auto &[tag, functions] : linked)
            for (id f in functions)
                if (!addStage(r, ids, seen, tag, f, bytes, every))
                    return false;
    }
    if (r.stages.size() > kRequestStageLimit)
        return false;
    name = storeName(k, r, ids);
    return true;
}

RecordCache::Found Completion::fromStore(const Digest &name, const CacheKey &k, RuntimeRecord &record, std::string &why)
{
    std::string bytes;
    if (!remote_->find(name, bytes))
        return RecordCache::Found::None;
    return decodeEntry(bytes, k, record, why);
}

// The wait split where it went (obtained.h; -1 for a part not measured): the request queued until its compile
// started, the compile, the compiler's answer after it, this thread's wake after that, and the store lookup. The split
// stays inside the parentheses: tools/gpu/logharvest.py reads the name and key after "for a compile slot: ".
void Completion::logRemote(const Ask &a, const CacheKey &k, RecordCache::Found found, const char *how,
                           const Obtained &o)
{
#if SWITCH_METAL_DIAGNOSTICS
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
#else
    (void)a;
    (void)k;
    (void)found;
    (void)how;
    (void)o;
#endif
}

bool Completion::remoteNow(const Ask &a, const CacheKey &k, Compiler::State &state, RuntimeRecord &record,
                           std::string &failure)
{
    CompileRequest r;
    Digest name;
    if (!renderRequest(a, k, false, r, name))
        return failure = kInexpressible, false;
    std::string why;
    RecordCache::Found found = fromStore(name, k, record, why);
    const uint64_t began = nowUs();
    SWITCH_METAL_DIAGNOSTIC(tObtained = {"store", 0, uint32_t(qos_class_self()), 0});
    if (found == RecordCache::Found::None) {
        if (!remote_->taking())
            return failure = kNoCompilerComes, false;
        Remote::Answered answered;
        Remote::Answer answer = Remote::Answer::Unavailable;
        uint64_t sent = began;
        for (const bool again : {false, true}) {
            if (!renderRequest(a, k, true, r, name, again))
                return failure = kInexpressible, false;
            const std::string request = encodeRequest(r);
            SWITCH_METAL_DIAGNOSTIC(sent = nowUs());
            answer = remote_->compile(request, &answered);
            if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                found = fromStore(name, k, record, why);
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
#else
        (void)sent;
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
        logRemote(a, k, found, "by the shader compiler", tObtained);
    } else {
        logRemote(a, k, found, "from the shader compiler's store", tObtained);
    }
    compiler_->keep(k, found, record, why);
    state = found == RecordCache::Found::Record ? Compiler::State::Ready : Compiler::State::Refused;
    if (state == Compiler::State::Refused)
        failure = why;
    return true;
}

bool Completion::remoteLater(const Ask &a, const CacheKey &k, std::function<void(const RuntimeRecord *)> done,
                             bool again)
{
    CompileRequest r;
    Digest name;
    if (!renderRequest(a, k, true, r, name, again)) {
        done(nullptr);
        return true;
    }
    // the record as the process table keeps it, so it outlives this answer for place()
    const auto settle = [this, k, done](RecordCache::Found found, const RuntimeRecord &record, const std::string &why) {
        compiler_->keep(k, found, record, why);
        const RuntimeRecord *kept = nullptr;
        const Compiler::State state = compiler_->ask(
            k,
            [done](const RuntimeRecord *made, const std::string &) {
                done(made);
            },
            &kept);
        if (state == Compiler::State::Ready || state == Compiler::State::Refused)
            done(state == Compiler::State::Ready ? kept : nullptr);
    };
    RuntimeRecord record;
    std::string why;
    if (const RecordCache::Found found = fromStore(name, k, record, why); found != RecordCache::Found::None) {
        logRemote(a, k, found, "from the shader compiler's store", Obtained{"store", 0, uint32_t(qos_class_self())});
        settle(found, record, why);
        return true;
    }
    if (!remote_->taking()) {
        done(nullptr);
        return true;
    }
    const std::string request = encodeRequest(r);
    const uint64_t began = nowUs();
    const Ask asked = a;
    uint64_t cookie = 0;
    const bool sent = remote_->compileAsync(
        request, a.waited, a.qos,
        [this, asked, k, name, began, settle, done, again](Remote::Answer answer, const Remote::Answered &answered) {
            RuntimeRecord made;
            std::string refusal;
            const RecordCache::Found found = answer == Remote::Answer::Made || answer == Remote::Answer::Refused
                                                 ? fromStore(name, k, made, refusal)
                                                 : RecordCache::Found::None;
            switch (askNext(answer, found != RecordCache::Found::None, again, remote_->declined())) {
            case AskNext::Use: {
                Obtained o{"compiled", nowUs() - began, asked.qos, answered.how};
                const TegraGM20BAbiCompileTimes &t = answered.times;
                SWITCH_METAL_DIAGNOSTIC(
                    o.split(began, t.compile_start, t.compile_end, t.answered, answered.at, began + o.waitUs));
                o.answerPriority = t.answer_priority;
                logRemote(asked, k, found, "by the shader compiler", o);
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
    Digest name;
    std::vector<LibraryIdentity> ids;
    const auto request = [&](bool every) {
        r = {};
        ids.clear();
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
        std::map<const LibrarySource *, uint32_t> seen;
        if (q.textures.size() > kRequestBindLimit || q.samplers.size() > kRequestBindLimit ||
            !addKernelStages(r, ids, seen, facadeFunction, descriptor, true, every))
            return false;
        name = storeName(k, r, ids);
        return true;
    };
    const auto stored = [&]() {
        std::string bytes;
        if (!remote_->find(name, bytes))
            return RecordCache::Found::None;
        return decodeKernelEntry(bytes, k, record, why);
    };
    if (!request(false))
        return Compiler::Outcome::Failed;
    RecordCache::Found found = stored();
    AskFailure failure = AskFailure::None;
    for (const bool again : {false, true}) {
        if (found != RecordCache::Found::None || !remote_->taking() || (again && !request(true)))
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
#if SWITCH_METAL_DIAGNOSTICS
    logKernelHarvest(k, r, ids);
#endif
    return found == RecordCache::Found::Record ? Compiler::Outcome::Made : Compiler::Outcome::Refused;
}

bool Completion::storedReflection(const std::function<bool(bool, CompileRequest &, Digest &)> &request,
                                  const CacheKey &k, std::string &blob, std::string &why)
{
    CompileRequest r;
    Digest name;
    std::string text;
    bool made = false;
    const auto stored = [&] {
        std::string bytes;
        return remote_->find(name, bytes) && decodeReflectionEntry(bytes, k, made, text);
    };
    if (!request(false, r, name))
        return why = kInexpressible, false;
    bool found = stored();
    for (const bool again : {false, true}) {
        if (found || !remote_->taking() || (again && !request(true, r, name)))
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
        [&](bool every, CompileRequest &r, Digest &name) {
            return renderRequest(a, k, true, r, name, every, RequestKind::Reflection);
        },
        k, blob, why);
}

bool Completion::reflection(id function, MTLComputePipelineDescriptor *descriptor, const std::string &stateKey,
                            std::string &blob, std::string &why)
{
    const CacheKey k = computeReflectionKey(stack_, stateKey);
    return storedReflection(
        [&](bool every, CompileRequest &r, Digest &name) {
            r = {};
            r.stack = stack_;
            r.kind = RequestKind::Reflection;
            r.key = k.key;
            r.kernel.maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
            r.kernel.multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
            r.kernel.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);
            std::vector<LibraryIdentity> ids;
            std::map<const LibrarySource *, uint32_t> seen;
            if (!addKernelStages(r, ids, seen, function, descriptor, true, every))
                return false;
            name = storeName(k, r, ids);
            return true;
        },
        k, blob, why);
}

bool Completion::functionReflection(const char *tag, id function, const CacheKey &k, std::string &blob,
                                    std::string &why)
{
    return storedReflection(
        [&](bool every, CompileRequest &r, Digest &name) {
            r = {};
            r.stack = stack_;
            r.kind = RequestKind::Reflection;
            r.key = k.key;
            std::vector<LibraryIdentity> ids;
            std::map<const LibrarySource *, uint32_t> seen;
            if (!addStage(r, ids, seen, tag, function, true, every))
                return false;
            name = storeName(k, r, ids);
            return true;
        },
        k, blob, why);
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
    Digest name;
    const auto request = [&](bool bytes, bool every) {
        r = {};
        r.stack = stack_;
        r.kind = RequestKind::Facts;
        r.key = k.key;
        r.kernel.maxThreads = uint32_t(descriptor.maxTotalThreadsPerThreadgroup);
        r.kernel.multipleOfWidth = descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth;
        r.kernel.stageIn = stageInFieldsOf(descriptor.stageInputDescriptor);
        std::vector<LibraryIdentity> ids;
        std::map<const LibrarySource *, uint32_t> seen;
        if (!addKernelStages(r, ids, seen, facadeFunction, descriptor, bytes, every))
            return false;
        name = storeName(k, r, ids);
        return true;
    };
    std::string refusal;
    const auto stored = [&]() {
        std::string bytes;
        return remote_->find(name, bytes) ? decodeFactsEntry(bytes, k, out, refusal) : RecordCache::Found::None;
    };
    if (!request(false, false))
        return failure = kInexpressible, false;
    RecordCache::Found found = stored();
    Remote::Answer answer = Remote::Answer::Unavailable;
#if SWITCH_METAL_DIAGNOSTICS
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
#if SWITCH_METAL_DIAGNOSTICS
    os_log(gm20bLog(),
           "SwitchMetal: a compute state the pack lacks answered %{public}s in %llu ms: max %u width %u memory %u: "
           "%{public}.300s",
           fromStore ? "from the shader compiler's store" : "by the shader compiler",
           (unsigned long long)((nowUs() - began) / 1000), out.maxThreads, out.executionWidth, out.staticMemory,
           stateKey.c_str());
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
    const Digest name = storeName(k, r, {});
    std::string log;
    const auto stored = [&]() {
        std::string entry;
        return remote_->find(name, entry) ? decodeLibraryEntry(entry, k, out, log) : RecordCache::Found::None;
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
