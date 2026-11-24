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
#include "compiler_pool.h"
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
#if SWITCH_DEBUG
    uint64_t takenNs = 0, preparedNs = 0;  // the kernel handed it over, and it was prepared
#endif
};

#if SWITCH_DEBUG
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
    malloc_zone_pressure_relief(nullptr, 0);
#if SWITCH_DEBUG
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
    std::unique_ptr<Pool> pool_;
    size_t bufferBytes_ = 1u << 20;
    std::unique_ptr<uint8_t[]> buffer_ = untouched(bufferBytes_);

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

    int32_t run(Work &work);
    void remember(const Asked &asked, int32_t ran);
    bool publish(const gm20b::Digest *name, const gm20b::CacheKey &key, const std::string &entry, bool slots,
                 std::string &why);
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

// The entry under `name` (null: the store holds it already), and with `slots` its program slots ahead of it
// (Store::putProgramSlots): a full store is written again with its newest half and registered again, then takes both
bool Service::publish(const gm20b::Digest *name, const gm20b::CacheKey &key, const std::string &entry, bool slots,
                      std::string &why)
{
    std::lock_guard<std::mutex> l(publishLock_);
    const auto putAll = [&] {
        if (slots && store_.putProgramSlots(key, entry) == Store::Put::Full)
            return Store::Put::Full;
        return name ? store_.put(*name, entry) : Store::Put::Added;
    };
    Store::Put put = putAll();
    if (put == Store::Put::Full) {
        const uint32_t before = store_.entries();
        if (!store_.makeRoom(why) || !registerStore(uint64_t(TEGRA_STACK_KEY), why))
            return false;
        os_log(logger(), "SwitchShaderCompiler: the full store kept its newest %u of %u entries", store_.entries(),
               before);
        put = putAll();
    }
    if (put != Store::Put::Added)
        why = "the store took no entry";
    return put == Store::Put::Added;
}

int32_t Service::run(Work &work)
{
    Asked &asked = static_cast<Asked &>(work);
    const Prepared &p = asked.prepared;
    const qos_class_t workerClass = qos_class_self();
#if SWITCH_DEBUG
    tJobSplit = {};
#endif
    std::string entry, why;
    if (store_.find(p.name, entry)) {
        const int32_t stored = storedResult(entry, p.key);
        if (stored >= 0) {
            noCompile();
            return packed(stored, true, workerClass);
        }
        store_.retire(p.name);
    }
    entry.clear();
    compileStarted();
    int32_t result = engine_.compile(p, entry, why);
#if SWITCH_DEBUG
    const uint64_t publishing = uptimeNs();
#endif
    if (!entry.empty() && !publish(&p.name, p.key, entry, result == TegraGM20BCompileMade, why))
        result = TegraGM20BCompileFailed;
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
#if SWITCH_DEBUG
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
    if ((result == TegraGM20BCompileMade || result == TegraGM20BCompileRefused) && !asked.request.empty())
        log_.add(asked.prepared.name, asked.request);
    if (result == TegraGM20BCompileRefused)
        refusals_->confirm(asked.prepared.name);
}

// True: a job of its own runs it, which signals `done` once it ran
bool Service::submit(Prepared prepared, std::string request, const Waiter &waiter, int32_t pid,
                     dispatch_semaphore_t done)
{
    std::string bytes;
    if (store_.find(prepared.name, bytes)) {
        const int32_t stored = storedResult(bytes, prepared.key);
        if (stored >= 0) {
            complete(waiter, stored, TegraGM20BCompileStored);
            return false;
        }
        store_.retire(prepared.name);
    }
    auto work = std::make_unique<Asked>();
    work->pid = pid;
    work->request = std::move(request);
    work->done = done;
#if SWITCH_DEBUG
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
    if (error || !reply.id) {
        os_log_error(logger(), "SwitchShaderCompiler: no request taken (0x%x): the kernel let go of the compiler",
                     error);
        exit(1);
    }
#if SWITCH_DEBUG
    tTakenNs = uptimeNs();
    dieIfArmedFor(reply.pid);
#endif
    const bool waited = reply.flags & TegraGM20BCompileWaited;
    std::string request(reinterpret_cast<const char *>(buffer_.get()), size_t(reply.bytes));
    const Waiter waiter = {reply.id, reply.client, bandOf(reply.qos, waited), false};
    Prepared prepared;
    std::string why;
    if (!prepare(request, stack, system_, prepared, why)) {
        os_log_error(logger(), "SwitchShaderCompiler: pid %d's request not taken: %{public}s", reply.pid, why.c_str());
        return complete(waiter, TegraGM20BCompileUnavailable, TegraGM20BCompileCompiled);
    }
    submit(std::move(prepared), std::move(request), waiter, reply.pid);
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
#if SWITCH_DEBUG
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
        if (!prepare(restacked(request, stack), stack, system_, prepared, why)) {
            dropped++;
            return false;
        }
        name = prepared.name;
        const gm20b::CacheKey key = prepared.key;
        std::string bytes;
        const int32_t stored = store_.find(name, bytes) ? storedResult(bytes, key) : -1;
        const bool unconfirmed = stored == TegraGM20BCompileRefused && !confirmed.confirmed(name);
        if (stored < 0 || unconfirmed) {
            if (unconfirmed)
                store_.retire(name);
            (unconfirmed ? refusals : again)++;
            if (submit(std::move(prepared), std::string(), {0, 0, Band::Background, false}, 0, ran))
                dispatch_semaphore_wait(ran, DISPATCH_TIME_FOREVER);
        } else if (stored == TegraGM20BCompileMade && !publish(nullptr, prepared.key, bytes, true, why)) {
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
    Pool::Hooks hooks;
    hooks.run = [this](Work &work, const std::function<void(int32_t)> &primary) {
        const int32_t result = run(work);
        primary(result);
        const Asked &asked = static_cast<Asked &>(work);
        remember(asked, result);
        if (asked.done)
            dispatch_semaphore_signal(asked.done);
        return result;
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
    static switch_compiler::Service service;
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
