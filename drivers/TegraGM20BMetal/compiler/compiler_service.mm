// SPDX-License-Identifier: MIT
// The system shader compiler's process (TegraGM20BCompilerABI.h), run by /usr/libexec/SwitchShaderCompiler from inside the
// GPU plugin. Once the GPU's service is up it reads the installed pack, opens its store (compiler_store.h) and registers it
// with the kernel, so every GPU client reads the store from then on; then it enters its sandbox, makes the requests an
// earlier build's store had (compiler_log.h) again at the background class, and takes
// requests one by one on a thread that sleeps in the kernel between them. A request is prepared there (compiler_request.h),
// answered from the store when its entry is in, and otherwise handed to the pool (compiler_pool.h), which makes each
// record once for every process that asks, at the class of its most urgent waiter. What it compiles with is made at the
// first compile (compiler_engine.h), inside the sandbox: a compiler whose store holds every state asked of it never makes
// one.
#import <Foundation/Foundation.h>

#include <IOKit/IOKitLib.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <os/log.h>
#include <pthread/qos.h>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "TegraGM20BABI.h"
#include "TegraGM20BCompilerABI.h"
#include "compiler_debug.h"
#include "compiler_engine.h"
#include "compiler_log.h"
#include "compiler_persist.h"
#include "compiler_pool.h"
#include "compiler_record.h"
#include "compiler_refusals.h"
#include "compiler_restart.h"
#include "compiler_sandbox.h"
#include "compiler_store.h"
#include "compiler_times.h"
#include "../../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../cache_io.h"
#include "../reflection_blob.h"
#include "../compute_facts_entry.h"
#include "../library_entry.h"

namespace switch_compiler {
namespace {

// Root's on iOS 27, where launchd starts the compiler as root; mobile's own on iOS 17, where the BlueTool stand-in (which
// runs as mobile) starts it
constexpr const char *kSystemRoot = "/private/var/Switch/shaders";
constexpr const char *kMobileRoot = "/private/var/mobile/Library/com.brunostjohn.switch.shaders";

os_log_t logger()
{
    static os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "compiler");
    return l;
}

// launchd's KeepAlive (iOS 27) starts the compiler again whenever it dies; the BlueTool stand-in (iOS 17) sets
// SWITCH_SHADER_COMPILER_LAST_START on the start after which it starts none
uint32_t restarts()
{
    return getenv("SWITCH_SHADER_COMPILER_LAST_START") ? 0u : uint32_t(TegraGM20BCompilerRestarts);
}

struct Asked : Work {
    Prepared prepared;
    std::string request;  // what the log keeps of a client's request ("": the log's own, which it keeps already)
    int32_t pid = 0;      // the process that asked first, whose share of the store the record takes
    dispatch_semaphore_t done = nullptr;  // signalled once it ran, for the replay that waits on it
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    uint64_t takenNs = 0, preparedNs = 0;  // the kernel handed it over, and it was prepared
#endif
};

#if SWITCH_DEBUG && SWITCH_METAL_STATS
// When the take thread's request came out of the kernel (0: the replay's own)
thread_local uint64_t tTakenNs;
#endif

// What a stored entry answers, -1 for one that is not `k`'s whole, or a library the front end refused (retired, and made
// again: the store is not named by what the front end ran with)
int32_t storedResult(const std::string &bytes, const gm20b::CacheKey &k)
{
    gm20b::Reader r(bytes);
    uint32_t kind = 0;
    if (!gm20b::readEntryHead(bytes, r, k, kind) || kind == gm20b::kKindLibraryRefusal)
        return -1;
    return kind == gm20b::kKindRecord || kind == gm20b::kKindKernel || kind == gm20b::kKindFacts ||
                   kind == gm20b::kKindLibrary || kind == gm20b::kKindReflection
               ? TegraGM20BCompileMade
               : TegraGM20BCompileRefused;
}

// Take's buffer, left uninitialised: zeroing it would touch every page, which jetsam counts, and Take writes only a
// request's own bytes
std::unique_ptr<uint8_t[]> untouched(size_t bytes)
{
    return std::unique_ptr<uint8_t[]>(new uint8_t[bytes]);  // NOLINT(modernize-make-unique): make_unique zeroes it
}

// The heap the workers' compiles freed goes back to the system once the pool has nothing queued or running
void idle()
{
    // Avoid holding libmalloc zone locks at background QoS while interactive compiles allocate.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
    malloc_zone_pressure_relief(nullptr, 0);
    pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    logFootprint(logger(), "idle");
#endif
}

class Service {
public:
    void serve();

private:
    io_connect_t connection_ = 0;
    std::string root_;
    Engine engine_;
    Store store_;
    RequestLog log_;
    std::unique_ptr<ConfirmedRefusals> refusals_;  // this build's refusals (compiler_refusals.h), made at open
    SystemLibraries system_;
    std::unique_ptr<ClientLibraries> clientLibraries_;
    std::unique_ptr<Pool> pool_;
    size_t bufferBytes_ = 1u << 20;
    std::unique_ptr<uint8_t[]> buffer_ = untouched(bufferBytes_);
    Persister persister_{[this] {
        persist();
    }};

    int call(uint32_t method, const void *in, size_t inSize, void *out, size_t outSize)
    {
        size_t outBytes = outSize;
        return IOConnectCallStructMethod(connection_, method, in, inSize, outSize ? out : nullptr,
                                         outSize ? &outBytes : nullptr);
    }

    void complete(const Waiter &waiter, int32_t result, uint32_t how, Band ran = Band::Background)
    {
        if (!waiter.id)
            return;
        const TegraGM20BAbiCompilerComplete done = {waiter.id, result, how | uint32_t(ran) << 8, answering()};
        if (const int error = call(TegraGM20BMethodCompilerComplete, &done, sizeof done, nullptr, 0);
            error && error != kIOReturnNotFound)
            os_log_error(logger(), "SwitchShaderCompiler: completing request %llu: 0x%x", (unsigned long long)waiter.id,
                         error);
    }

    std::mutex publishLock_;

    void persist();

    // Where `p`'s entry is (Store::find over its candidate names): `bytes`, and `stored` its result, -1 for none or one to
    // make again. A publish chooses again as it writes (Store::put), since another content may have taken the name
    // meanwhile.
    struct Placed {
        gm20b::Digest name{};
        int32_t stored = -1;
        std::string bytes;
    };

    Placed placed(const Prepared &p) const;
    int32_t run(Work &work);
    void remember(const Asked &asked, int32_t ran);
    bool publish(gm20b::Digest *name, const gm20b::CacheKey &key, const std::string &entry, bool slots, bool keepMade,
                 std::string &why);
    bool filedAsPlain(const Prepared &p, const std::string &entry, bool write);
    bool connect(std::string &why, StartFailure &failure);
    void decline();
    bool kernelGaveUp();
    bool registerStore(uint64_t stack, std::string &why, StartFailure *failure = nullptr);
    void open(uint64_t stack);
    [[noreturn]] void notServing(const std::string &why, StartFailure failure);
    void warm(uint64_t stack);
    void take(uint64_t stack);
    bool submit(Prepared prepared, std::string request, const Waiter &waiter, int32_t pid,
                dispatch_semaphore_t done = nullptr);
};

// A job's result as the pool hands it to each waiter: TegraGM20BCompilerABI.h's result, whether the store held it when
// the job ran (an earlier job of the same record published it after this one was submitted), and the class it ran at
constexpr int32_t kStoredBit = 1 << 8, kClassShift = 16;

int32_t packed(int32_t result, bool stored, qos_class_t ran)
{
    return result | (stored ? kStoredBit : 0) | int32_t(bandOf(uint32_t(ran), true)) << kClassShift;
}

// The entry of `key` placed from `name` (Store::put; null: the store holds it already), `name` then the one it went
// under, and with `slots` its program slots ahead of it. `keepMade`: a made entry of `key` already there stays (a
// compile's result: one content compiles to one record, and the one there may be a full form's, filedAsPlain)
// (Store::putProgramSlots): a full store is written again with its newest half and registered again, then takes both
bool Service::publish(gm20b::Digest *name, const gm20b::CacheKey &key, const std::string &entry, bool slots,
                      bool keepMade, std::string &why)
{
    std::lock_guard<std::mutex> l(publishLock_);
    const auto putAll = [&] {
        if (slots && store_.putProgramSlots(key, entry) == Store::Put::Full)
            return Store::Put::Full;
        if (!name)
            return Store::Put::Added;
        gm20b::Digest at{};
        const Store::Put put = store_.put(*name, key, entry, at, [&](const std::string &held) {
            return keepMade && storedResult(held, key) == TegraGM20BCompileMade;
        });
        if (put == Store::Put::Added || put == Store::Put::Kept)
            *name = at;
        return put;
    };
    Store::Put put = putAll();
    if (put == Store::Put::Full) {
#if SWITCH_DEBUG
        const uint32_t before = store_.entries();
#endif
        if (!store_.makeRoom(why) || !registerStore(uint64_t(TEGRA_STACK_KEY), why))
            return false;
#if SWITCH_DEBUG
        os_log(logger(), "SwitchShaderCompiler: the full store kept its newest %u of %u entries", store_.entries(),
               before);
#endif
        put = putAll();
    }
    if (put != Store::Put::Added && put != Store::Put::Kept)
        why = "the store took no entry";
    if (put == Store::Put::Added)
        persister_.soon();
    return put == Store::Put::Added || put == Store::Put::Kept;
}

// Whether a full form's stored record `entry` is also what its plain content's lookup finds (asPrimary); with `write`,
// filed there where it is not. A plain compile racing the full one, or an entry an older build made, leaves the plain
// record under that content, and the state's creations would wait for their full form at every launch.
bool Service::filedAsPlain(const Prepared &p, const std::string &entry, bool write)
{
    const std::string filed = asPrimary(p, entry);
    std::string held;
    gm20b::Digest at{};
    if (filed.empty() || (store_.find(p.primaryName, p.primaryContent, held, at) && held == filed))
        return true;
    if (!write)
        return false;
    gm20b::Digest primary = p.primaryName;
    std::string ignored;
    return publish(&primary, p.primaryContent, filed, false, false, ignored);
}

void Service::persist()
{
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    const uint64_t began = uptimeNs();
#endif
    if (!store_.persist())
        os_log_error(logger(),
                     "SwitchShaderCompiler: the store not synced (%d): a crash before the next sync drops its newest "
                     "entries",
                     errno);
    if (!log_.persist())
        os_log_error(logger(), "SwitchShaderCompiler: the request log not synced (%d)", errno);
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    static uint32_t syncs = 0;  // the persister's serial queue alone runs this
    os_log(logger(), "SwitchShaderCompiler: store and request log synced in %llu us (sync %u)",
           (unsigned long long)((uptimeNs() - began) / 1000), ++syncs);
#endif
}

Service::Placed Service::placed(const Prepared &p) const
{
    Placed out;
    if (store_.find(p.name, p.content, out.bytes, out.name))
        out.stored = storedResult(out.bytes, p.content);
    return out;
}

int32_t Service::run(Work &work)
{
    Asked &asked = static_cast<Asked &>(work);
    Prepared &p = asked.prepared;
    const qos_class_t workerClass = qos_class_self();
#if SWITCH_DEBUG
    tJobSplit = {};
#endif
    std::string why;
    if (const Placed place = placed(p); place.stored >= 0) {
        p.name = place.name;
        if (gm20b::fullForm(p.request.form) && place.stored == TegraGM20BCompileMade)
            filedAsPlain(p, place.bytes, true);
        noCompile();
        return packed(place.stored, true, workerClass);
    }
    std::string entry;
    compileStarted();
    int32_t result = engine_.compile(p, entry, why);
#if SWITCH_DEBUG
    const uint64_t publishing = uptimeNs();
#endif
    if (!entry.empty() && !publish(&p.name, p.content, entry, result == TegraGM20BCompileMade, true, why))
        result = TegraGM20BCompileFailed;
    if (result == TegraGM20BCompileMade && gm20b::fullForm(p.request.form))
        filedAsPlain(p, entry, true);
#if SWITCH_DEBUG
    tJobSplit.storeNs = uptimeNs() - publishing;
#endif
    compileEnded();
    const bool refused = result == TegraGM20BCompileRefused || (result == TegraGM20BCompileFailed && failsForGood(why));
    if (!refused && (result == TegraGM20BCompileFailed || result == TegraGM20BCompileUnavailable))
        os_log_error(logger(), "SwitchShaderCompiler: pid %d's state not made: %{public}s | %{public}.300s", asked.pid,
                     why.c_str(), p.key.key.c_str());
    if (refused) {
        const RefusalLine line = refusalLine(p.request, asked.pid, why);
        switch_refusal_k(
            switch_refusal_key_with(switch_refusal_key("m2v", line.reason), switch_refusal_key(line.text.c_str(), "")),
            "m2v", line.reason, "%s", line.text.c_str());
    }
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    logJob(logger(), result, asked.takenNs, asked.preparedNs, unsigned(workerClass), asked.pid, p.key.key);
#endif
    return packed(result, false, workerClass);
}

// What a compile leaves besides its record, once its waiters have it: the request kept for the next build's store, and a
// refusal confirmed for this build
void Service::remember(const Asked &asked, int32_t ran)
{
    const int32_t result = ran & 0xff;
    if (ran & kStoredBit)
        return;
    // Scoped requests omit their scope from the log; front-end library requests are compiled by Apple's MTLCompiler
    // (already persisted across builds in msl/*.reply), so logging them would reload MTLCompiler and re-run syntax
    // errors on every boot.
    if ((result == TegraGM20BCompileMade || result == TegraGM20BCompileRefused) && !asked.request.empty() &&
        asked.prepared.request.kind != gm20b::RequestKind::Library && !gm20b::scoped(asked.prepared.request)) {
        log_.add(asked.prepared.name, asked.request);
        persister_.soon();
    }
    if (result == TegraGM20BCompileRefused)
        refusals_->confirm(asked.prepared.name);
}

// True: a job of its own runs it, which signals `done` once it ran
bool Service::submit(Prepared prepared, std::string request, const Waiter &waiter, int32_t pid,
                     dispatch_semaphore_t done)
{
    // a full form stored but not yet filed as its plain content's record goes to a worker, which files it
    if (const Placed place = placed(prepared);
        place.stored >= 0 && (!gm20b::fullForm(prepared.request.form) || place.stored != TegraGM20BCompileMade ||
                              filedAsPlain(prepared, place.bytes, false))) {
        complete(waiter, place.stored, TegraGM20BCompileStored);
        return false;
    }
    auto work = std::make_unique<Asked>();
    work->pid = pid;
    work->request = std::move(request);
    work->done = done;
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    work->takenNs = tTakenNs;
    work->preparedNs = uptimeNs();
#endif
    const gm20b::Digest name = prepared.name;
    work->prepared = std::move(prepared);
    return pool_->submit(name, std::move(work), waiter);
}

void Service::take(uint64_t stack)
{
    TegraGM20BAbiCompilerTake in = {uint64_t(uintptr_t(buffer_.get())), bufferBytes_};
    TegraGM20BAbiCompilerTakeReply reply = {};
    const int error = call(TegraGM20BMethodCompilerTake, &in, sizeof in, &reply, sizeof reply);
    if (!error && !reply.id && reply.bytes > bufferBytes_ && reply.bytes <= TegraGM20BCompileRequestLimit) {
        bufferBytes_ = size_t(reply.bytes);
        buffer_ = untouched(bufferBytes_);
        return;
    }
    if (error == kIOReturnAborted)
        return;
    if (error == kIOReturnBadArgument)
        notServing("the kernel refused this build's Take reply size: the GPU kext is of another build",
                   StartFailure::Structural);
    if (error || !reply.id) {
        os_log_error(logger(), "SwitchShaderCompiler: no request taken (0x%x): the kernel let go of the compiler",
                     error);
        exit(1);
    }
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    tTakenNs = uptimeNs();
#endif
#if SWITCH_DEBUG
    dieIfArmedFor(reply.pid);
#endif
    const bool waited = reply.flags & TegraGM20BCompileWaited;
    std::string request(reinterpret_cast<const char *>(buffer_.get()), size_t(reply.bytes));
    const Waiter waiter = {reply.id, reply.client, bandOf(reply.qos, waited), false};
    Prepared prepared;
    std::string why;
    gm20b::StoreScope scope;
    scope.kind = reply.scope.kind;
    scope.identity.assign(reinterpret_cast<const char *>(reply.scope.identity),
                          std::min<size_t>(reply.scope.bytes, sizeof reply.scope.identity));
    if (!prepare(request, stack, system_, prepared, why, reply.client, clientLibraries_.get(), scope)) {
        os_log_error(logger(), "SwitchShaderCompiler: pid %d's request not taken: %{public}s", reply.pid, why.c_str());
        return complete(waiter, TegraGM20BCompileUnavailable, TegraGM20BCompileCompiled);
    }
    bool hasHandles = false;
    for (const gm20b::RequestLibrary &l : prepared.request.libraries)
        if (l.libraryId) {
            hasHandles = true;
            break;
        }
    submit(std::move(prepared), hasHandles ? std::string() : std::move(request), waiter, reply.pid);
}

bool Service::connect(std::string &why, StartFailure &failure)
{
    io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"));
    const kern_return_t opened =
        service ? IOServiceOpen(service, mach_task_self(), TegraGM20BOpenType, &connection_) : KERN_FAILURE;
    if (service)
        IOObjectRelease(service);
    if (opened) {
        char text[64];
        snprintf(text, sizeof text, "no connection to the GPU (0x%x)", opened);
        failure = connectFailure(opened);
        return why = text, false;
    }
    const TegraGM20BAbiCompilerAnnounce in = {restarts(), 0};
    call(TegraGM20BMethodCompilerAnnounce, &in, sizeof in, nullptr, 0);
    return true;
}

// No compiler comes this boot: the kernel answers every waiting and later request Unavailable, and a client fails the
// creation of a state the pack, the seed and the store lack, by name (TegraGM20BMetalComplete.h kNoCompilerComes)
void Service::decline()
{
    if (connection_)
        call(TegraGM20BMethodCompilerDecline, nullptr, 0, nullptr, 0);
}

bool Service::kernelGaveUp()
{
    TegraGM20BAbiCompilerInfoReply info = {};
    return connection_ && !call(TegraGM20BMethodCompilerInfo, nullptr, 0, &info, sizeof info) &&
           (info.flags & TegraGM20BCompilerDeclined);
}

bool Service::registerStore(uint64_t stack, std::string &why, StartFailure *failure)
{
    TegraGM20BAbiCompilerRegister in = {uint64_t(uintptr_t(store_.base())), store_.mappedBytes(), stack, restarts(), 0};
    TegraGM20BAbiCompilerRegisterReply reply = {};
    const int registered = call(TegraGM20BMethodCompilerRegister, &in, sizeof in, &reply, sizeof reply);
    if (registered) {
        char text[64];
        snprintf(text, sizeof text, "the kernel refused its registration (0x%x)", registered);
        if (failure)
            *failure = registrationFailure(registered);
        return why = text, false;
    }
    os_log(logger(),
           "SwitchShaderCompiler: store registered at %llu ms of uptime: %{public}s, %u entries, generation %u, "
           "stack %016llx",
           (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000), store_.path().c_str(),
           store_.entries(), reply.generation, (unsigned long long)stack);
    return true;
}

[[noreturn]] void Service::notServing(const std::string &why, StartFailure failure)
{
    const Ending end = ending(failure, restarts() != 0, kernelGaveUp());
    os_log_error(logger(), "SwitchShaderCompiler: not serving: %{public}s: %{public}s", why.c_str(),
                 end == Ending::Restart ? "exits for its launcher to start it again"
                 : end == Ending::Leave ? "another compiler serves"
                                        : "declines for the boot");
    if (end == Ending::Decline)
        decline();
    exit(end == Ending::Restart ? 1 : 0);
}

void Service::open(uint64_t stack)
{
    root_ = geteuid() ? kMobileRoot : kSystemRoot;
    std::string why;
    EngineInputs inputs = EngineInputs::installed();
    inputs.codeGen.modules = root_ + "/modules";
    inputs.codeGen.replies = root_ + "/msl";
    StartFailure failure = StartFailure::Transient;
    if (!connect(why, failure))
        notServing(why, failure);
    if (!engine_.open(stack, inputs, why))
        notServing(why, StartFailure::Structural);
    if (!store_.open(root_, stack, engine_.rules(), why))
        notServing(why, StartFailure::Transient);
    if (!registerStore(stack, why, &failure))
        notServing(why, failure);
    refusals_ = std::make_unique<ConfirmedRefusals>(store_.path().substr(0, store_.path().rfind('/')),
                                                    imageBuild(reinterpret_cast<const void *>(&logger)));
    if (!log_.open(root_ + "/requests"))
        os_log_error(logger(), "SwitchShaderCompiler: no request log in %{public}s: an update compiles on demand",
                     root_.c_str());
    if (!enterSandbox(root_, why))
        os_log_error(logger(), "SwitchShaderCompiler: running without its sandbox: %{public}s", why.c_str());
    os_log(logger(), "SwitchShaderCompiler: serving at %llu ms of uptime",
           (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000));
#if SWITCH_DEBUG && SWITCH_METAL_STATS
    logFootprint(logger(), "serving");
#endif
}

// The requests an earlier store was made from, made again as background work nobody waits for, one at a time: the pool
// runs one background compile at once, and a queued request would hold its libraries' bytes until it ran. One an
// earlier build logged is made under this build's stack key, so the states a device made before a key moved are made
// again rather than dropped; one of another layout, or whose fields this build keys otherwise, is dropped. A store
// already holding one gets the record's program slots it lacks (a store written before they were kept, or one that
// dropped them). A stored refusal this compiler build has not confirmed is made again too (compiler_refusals.h).
void Service::warm(uint64_t stack)
{
    uint32_t again = 0, dropped = 0, refusals = 0;
    ConfirmedRefusals &confirmed = *refusals_;
    dispatch_semaphore_t ran = dispatch_semaphore_create(0);
    log_.replay([&](const std::string &request, gm20b::Digest &name) {
        Prepared prepared;
        std::string why;
        // Drop requests that no longer prepare, scoped requests (scope not in log), and front-end library requests
        // (compiled by Apple's MTLCompiler; replaying them loads MTLCompiler and re-runs syntax errors every boot).
        if (!prepare(restacked(request, stack), stack, system_, prepared, why) ||
            prepared.request.kind == gm20b::RequestKind::Library || gm20b::scoped(prepared.request)) {
            dropped++;
            return false;
        }
        const Placed place = placed(prepared);
        name = place.name;
        const std::string &bytes = place.bytes;
        const int32_t stored = place.stored;
        const bool unconfirmed = stored == TegraGM20BCompileRefused && !confirmed.confirmed(name);
        if (stored < 0 || unconfirmed) {
            if (unconfirmed)
                store_.retire(name, prepared.content);
            (unconfirmed ? refusals : again)++;
            if (submit(std::move(prepared), std::string(), {0, 0, Band::Background, false, false, true}, 0, ran))
                dispatch_semaphore_wait(ran, DISPATCH_TIME_FOREVER);
        } else if (stored == TegraGM20BCompileMade && !publish(nullptr, prepared.content, bytes, true, false, why)) {
            os_log_error(logger(), "SwitchShaderCompiler: a stored record's program slots not kept: %{public}s",
                         why.c_str());
        }
        return true;
    });
    if (again || dropped || refusals)
        os_log(logger(),
               "SwitchShaderCompiler: %u states an earlier build made are made again in the background, %u "
               "refusals this build had not confirmed, %u requests dropped",
               again, refusals, dropped);
}

void Service::serve()
{
    const uint64_t stack = uint64_t(TEGRA_STACK_KEY);
#if SWITCH_DEBUG
    armDeath();  // before the sandbox, which looks up no notification center
#endif
    open(stack);
    clientLibraries_ = std::make_unique<ClientLibraries>(
        [this](uint64_t client, uint64_t id, const uint8_t *&data, size_t &size, std::string &why) {
            const TegraGM20BAbiCompilerMapLibrary in = {id, client, 0, 0};
            TegraGM20BAbiCompilerMapLibraryReply reply = {};
            const int error = call(TegraGM20BMethodCompilerMapLibrary, &in, sizeof in, &reply, sizeof reply);
            if (error || !reply.address || !reply.bytes) {
                char text[64];
                snprintf(text, sizeof text, "client library map failed (0x%x)", error);
                why = text;
                return false;
            }
            data = reinterpret_cast<const uint8_t *>(uintptr_t(reply.address));
            size = size_t(reply.bytes);
            return true;
        },
        [this](uint64_t client, uint64_t id) {
            const TegraGM20BAbiCompilerUnmapLibrary in = {id, client, 0, 0};
            const int error = call(TegraGM20BMethodCompilerUnmapLibrary, &in, sizeof in, nullptr, 0);
            if (error && error != kIOReturnNotFound)
                os_log_error(logger(), "SwitchShaderCompiler: unmapping client %llu library %llu: 0x%x",
                             (unsigned long long)client, (unsigned long long)id, error);
        });
    Pool::Hooks hooks;
    hooks.run = [this](Work &work, const std::function<void(int32_t)> &primary) {
        const Asked &asked = static_cast<Asked &>(work);
        // Keep live client async/background compiles out of DarwinBG priority 4 while staying in the background lane.
        const bool lifted = asked.pid != 0 && qos_class_self() == QOS_CLASS_BACKGROUND;
        if (lifted)
            pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
        const int32_t result = run(work);
        primary(result);
        if (lifted)
            pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
        remember(asked, result);
        if (asked.done)
            dispatch_semaphore_signal(asked.done);
        return result;
    };
    hooks.same = [](const Work &a, const Work &b) {
        const gm20b::CacheKey &x = static_cast<const Asked &>(a).prepared.content,
                              &y = static_cast<const Asked &>(b).prepared.content;
        return x.stack == y.stack && !memcmp(x.vertexAir, y.vertexAir, sizeof x.vertexAir) &&
               !memcmp(x.fragmentAir, y.fragmentAir, sizeof x.fragmentAir) && x.key == y.key;
    };
    hooks.complete = [this](const Waiter &waiter, int32_t result) {
        const uint32_t how = result & kStoredBit ? TegraGM20BCompileStored
                             : waiter.joined     ? TegraGM20BCompileJoined
                                                 : TegraGM20BCompileCompiled;
        complete(waiter, result & 0xff, how, Band((result >> kClassShift) & 0xff));
    };
    hooks.setBand = [](Band band) {
        pthread_set_qos_class_self_np(qos_class_t(qosOf(band)), 0);
    };
    hooks.raise = [](pthread_t worker, Band band) -> void * {
        return pthread_override_qos_class_start_np(worker, qos_class_t(qosOf(band)), 0);
    };
    hooks.lower = [](void *token) {
        pthread_override_qos_class_end_np(static_cast<pthread_override_t>(token));
    };
    hooks.registerWorker = [this](uint32_t worker) {
        const TegraGM20BAbiCompilerServe in = {0, worker, 0};
        call(TegraGM20BMethodCompilerServe, &in, sizeof in, nullptr, 0);
    };
    hooks.serve = [this](uint64_t id, uint32_t worker) {
        const TegraGM20BAbiCompilerServe in = {id, worker, 0};
        call(TegraGM20BMethodCompilerServe, &in, sizeof in, nullptr, 0);
    };
    hooks.idle = idle;
    pool_ = std::make_unique<Pool>(std::move(hooks));
    std::thread([this] {
        pthread_set_qos_class_self_np(QOS_CLASS_BACKGROUND, 0);
        warm(uint64_t(TEGRA_STACK_KEY));
    }).detach();
    for (;;)
        take(stack);
}

}  // namespace
}  // namespace switch_compiler

// The GPU's service may be published after this process starts (the kernel publishes it once the GPU is up): the compiler
// starts at its first match, never by asking again. The thread that takes requests runs at the user-interactive class:
// it only sleeps in the kernel and hands each request on, and a compile runs at its waiters' class on a pool worker.
extern "C" __attribute__((visibility("default"))) int switch_shader_compiler_main(void)
{
    // Never destroyed: the take thread, the pool's workers and a pending sync of the store still run while exit()
    // runs static destructors
    static switch_compiler::Service &service = *new switch_compiler::Service;
    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    IONotificationPortSetDispatchQueue(port, dispatch_get_main_queue());
    io_iterator_t matches = 0;
    static bool started;
    const auto matched = [](void *, io_iterator_t iterator) {
        while (io_object_t found = IOIteratorNext(iterator)) {
            IOObjectRelease(found);
            if (started)
                continue;
            started = true;
            std::thread([] {
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
                service.serve();
            }).detach();
        }
    };
    if (IOServiceAddMatchingNotification(port, kIOFirstMatchNotification, IOServiceMatching("AGXAccelerator"), matched,
                                         nullptr, &matches) != KERN_SUCCESS) {
        os_log_error(OS_LOG_DEFAULT, "SwitchShaderCompiler: cannot watch for the GPU's service");
        return 0;
    }
    matched(nullptr, matches);
    dispatch_main();
}
