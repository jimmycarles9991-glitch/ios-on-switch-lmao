// SPDX-License-Identifier: MIT
// Completion's side of the system shader compiler (raw_remote.h): a state's or kernel's request made of the facade's own
// functions and libraries, its record looked up in the compiler's store, and asked of the compiler when the store lacks it.
#import "RawComplete.h"

#include <os/log.h>
#include <pthread/qos.h>
#include "NativeBridgeMetalFrontend.h"
#include "RawCompileRequest.h"
#include "RawMetalInternal.h"
#include "raw_compute_record.h"
#include "raw_remote_ask.h"

namespace raw {
namespace {

#if SWITCH_RAW_DIAGNOSTICS
const char *classText(uint32_t qos)
{
    return qos == QOS_CLASS_USER_INTERACTIVE ? "UI"
           : qos == QOS_CLASS_USER_INITIATED ? "IN"
           : qos == QOS_CLASS_DEFAULT        ? "DF"
           : qos == QOS_CLASS_UTILITY        ? "UT"
           : qos == QOS_CLASS_BACKGROUND     ? "BG"
                                             : "fixed";
}
#endif

// A function of this facade as a request's stage, its library added to `r` once (`ids` beside it); false: one no request
// can name (another device's, or one neither the facade nor m2v keeps a metallib of). The bytes are the AIR slice the
// facade read, else m2v's library's; `every` sends a system library's too, as a data library.
bool addStage(CompileRequest &r, std::vector<LibraryIdentity> &ids, std::map<const LibrarySource *, uint32_t> &seen,
              const char *tag, id function, bool bytes, bool every = false)
{
    if (![function isKindOfClass:[RMFunction class]])
        return false;
    RMFunction *f = function;
    const void *data = f->_source ? f->_source->bytes : nullptr;
    size_t length = f->_source ? f->_source->size : 0;
    if (!f->_source || (!data && !native_bridge_metal_function_metallib(f->_twin, &data, &length)))
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

void Completion::compiledHere(InProcess::Why why)
{
    if (inProcess_.compiled(why))
        os_log_error(rawLog(), "raw: %{public}s compiles a state itself: %{public}s", getprogname(),
                     InProcess::text(why));
}

bool Completion::renderRequest(const Ask &a, const CacheKey &k, bool bytes, CompileRequest &r, Digest &name, bool every)
{
    MTLRenderPipelineDescriptor *d = a.facade;
    if (!d)
        return false;
    r = {};
    r.stack = stack_;
    r.key = k.key;
    r.fields = fieldsOf(d);
    r.cubic = a.cubic;
    r.fragmentTests = a.fragmentTests;
    std::vector<LibraryIdentity> ids;
    std::map<const LibrarySource *, uint32_t> seen;
    @autoreleasepool {
        if (!addStage(r, ids, seen, "v", d.vertexFunction, bytes, every) ||
            !addStage(r, ids, seen, "f", d.fragmentFunction, bytes, every))
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

void Completion::logRemote(const Ask &a, const CacheKey &k, RecordCache::Found found, const char *how,
                           uint64_t waitedUs)
{
#if SWITCH_RAW_DIAGNOSTICS
    const bool made = found == RecordCache::Found::Record;
    os_log(rawLog(),
           "raw: a state the pack lacks %{public}s in %llu ms (%{public}s, %{public}s) after 0 ms for a compile slot: "
           "%016llx %{public}.400s",
           made ? "made" : "refused", (unsigned long long)(waitedUs / 1000), classText(qos_class_self()), how,
           (unsigned long long)k.name(), k.key.c_str());
    logHarvestParts(k.name(), a.descriptor.label ?: a.facade.label, k.key);
#else
    (void)a;
    (void)k;
    (void)found;
    (void)how;
    (void)waitedUs;
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
    RAW_DIAGNOSTIC(tObtained = {"store", 0, uint32_t(qos_class_self()), 0});
    if (found == RecordCache::Found::None) {
        if (!remote_->taking())
            return failure = kNoCompilerComes, false;
        uint32_t how = 0;
        Remote::Answer answer = Remote::Answer::Unavailable;
        for (const bool again : {false, true}) {
            if (!renderRequest(a, k, true, r, name, again))
                return failure = kInexpressible, false;
            answer = remote_->compile(encodeRequest(r), &how);
            if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
                found = fromStore(name, k, record, why);
            if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
                break;
        }
#if SWITCH_RAW_DIAGNOSTICS
        static const char *const kinds[] = {"compiled", "joined", "stored"};
        tObtained.how = (how & 0xff) < 3 ? kinds[how & 0xff] : "?";
        tObtained.waitUs = nowUs() - began;
        tObtained.ran = how;
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
        logRemote(a, k, found, "by the shader compiler", nowUs() - began);
    } else {
        logRemote(a, k, found, "from the shader compiler's store", 0);
    }
    compiler_->keep(k, found, record, why);
    state = found == RecordCache::Found::Record ? Compiler::State::Ready : Compiler::State::Refused;
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
        logRemote(a, k, found, "from the shader compiler's store", 0);
        settle(found, record, why);
        return true;
    }
    if (!remote_->taking()) {
        done(nullptr);
        return true;
    }
    const uint64_t began = nowUs();
    const Ask asked = a;
    const bool sent = remote_->compileAsync(
        encodeRequest(r), true, a.qos, [this, asked, k, name, began, settle, done, again](Remote::Answer answer) {
            RuntimeRecord made;
            std::string refusal;
            const RecordCache::Found found = answer == Remote::Answer::Made || answer == Remote::Answer::Refused
                                                 ? fromStore(name, k, made, refusal)
                                                 : RecordCache::Found::None;
            switch (askNext(answer, found != RecordCache::Found::None, again, remote_->declined())) {
            case AskNext::Use:
                logRemote(asked, k, found, "by the shader compiler", nowUs() - began);
                return settle(found, made, refusal);
            case AskNext::AskAgain:
                return (void)remoteLater(asked, k, done, true);
            case AskNext::End:
                return done(nullptr);
            }
        });
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
    const auto request = [&](bool every) {
        r = {};
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
        std::vector<LibraryIdentity> ids;
        std::map<const LibrarySource *, uint32_t> seen;
        if (q.textures.size() > kRequestBindLimit || q.samplers.size() > kRequestBindLimit ||
            !addStage(r, ids, seen, "k", facadeFunction, true, every))
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
    for (const bool again : {false, true}) {
        if (found != RecordCache::Found::None || !remote_->taking() || (again && !request(true)))
            break;
        const Remote::Answer answer = remote_->compile(encodeRequest(r));
        if (answer == Remote::Answer::Made || answer == Remote::Answer::Refused)
            found = stored();
        if (askFailure(answer, found != RecordCache::Found::None, false) == AskFailure::CouldNotRun) {
            taken = true;
            return Compiler::Outcome::Failed;
        }
        if (askNext(answer, found != RecordCache::Found::None, again, remote_->declined()) != AskNext::AskAgain)
            break;
    }
    if (found == RecordCache::Found::None)
        return Compiler::Outcome::Failed;
    taken = true;
    return found == RecordCache::Found::Record ? Compiler::Outcome::Made : Compiler::Outcome::Refused;
}

}  // namespace raw
