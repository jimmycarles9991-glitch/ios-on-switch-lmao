// SPDX-License-Identifier: MIT
// What the system shader compiler holds as it serves boot 12's requests (compiler_boot12.log), each run a process of its
// own over the mock GM20B, as compiler_service.mm opens and serves: the pack, the store, the request log an earlier boot
// left (boot 12's 13 replayed requests), the take buffer and the pool's workers; the log replayed at the background
// class; the 2 requests clients then asked; the compiler at rest (its libraries let go: compiler_libraries.h), and the
// heap relieved (malloc_zone_pressure_relief). Once with the m2v device the compiler opened before its compile
// context (its libraries cached as compiler_engine.mm cached them), once with the engine. Each step's line: the heap in
// use and allocated (the device's footprint line's figures), phys_footprint and its peak, and what grew, by image and VM
// tag (footprint_breakdown.h). Run by hand: SWITCH_HWTEST_METALLIB names the HW Test app's library (its states are left
// out without it), SWITCH_PACK a raw pack, and with MallocStackLogging=1, COMPILER_FOOTPRINT_SITES=<dir> keeps
// malloc_history's allocations by size and vmmap's summary at each step there.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <malloc/malloc.h>
#include <pthread/qos.h>
#include "NativeBridgeMetalFrontend.h"
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "footprint_breakdown.h"
#include "../compiler/compiler_engine.h"
#include "../compiler/compiler_log.h"
#include "../compiler/compiler_pool.h"
#include "../compiler/compiler_store.h"
#include "../raw/runtime/raw_pack.h"

#include <sys/stat.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>
#include <mutex>
#include <thread>
#include "TegraGM20BCompilerABI.h"

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string loader()
{
    char path[PATH_MAX];
    const char *nvk = getenv("SWITCH_NVK");
    return nvk && realpath(nvk, path) ? path : "";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

std::string packPath()
{
    const char *pack = getenv("SWITCH_PACK");
    return pack && *pack ? pack : compiler_test::emptyPack(dir());
}

// What the device's line reports (compiler_service.mm logFootprint on x1-jetsam), and what grew since the last step
struct Steps {
    const char *mode;
    std::map<std::string, int64_t> owners = footprint::dirtyByOwner();

    void step(const char *what)
    {
        task_vm_info_data_t vm = {};
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
        malloc_statistics_t heap = {};
        malloc_zone_statistics(nullptr, &heap);
        const std::map<std::string, int64_t> now = footprint::dirtyByOwner();
        char line[256];
        snprintf(line, sizeof line,
                 "%s %s: heap %llu KiB in use of %llu KiB; footprint %llu KiB, peak %llu KiB; grew:", mode, what,
                 (unsigned long long)(heap.size_in_use >> 10), (unsigned long long)(heap.size_allocated >> 10),
                 (unsigned long long)(vm.phys_footprint >> 10),
                 (unsigned long long)(vm.ledger_phys_footprint_peak >> 10));
        std::ofstream(dir() + "/" + mode + ".txt", std::ios::app) << line << footprint::growth(owners, now) << "\n";
        owners = now;
        if (const char *sites = getenv("COMPILER_FOOTPRINT_SITES")) {
            const std::string out = std::string(sites) + "/" + mode + "-" + what, pid = std::to_string(getpid());
            system(("/usr/bin/malloc_history " + pid + " -allBySize > '" + out + ".sites' 2>&1").c_str());
            system(("/usr/bin/vmmap -summary " + pid + " > '" + out + ".vmmap' 2>&1").c_str());
        }
    }
};

void show(const char *mode)
{
    std::ifstream in(dir() + "/" + mode + ".txt");
    for (std::string line; std::getline(in, line);)
        printf("%s\n", line.c_str());
}

struct Asked : switch_compiler::Work {
    switch_compiler::Prepared prepared;
    std::string request;
    dispatch_semaphore_t done = nullptr;
};

std::string logPath()
{
    return dir() + "/shaders/requests";
}

// Boot 12's requests (compiler_boot12.log): the request log's, which an earlier boot's compiles left, and the 2 asked
std::vector<raw::CompileRequest> boot12()
{
    compiler_test::Requests made(
        root(), {{"/Applications/SwitchHWTest.app/default.metallib", getenv("SWITCH_HWTEST_METALLIB") ?: ""}});
    return made.of(compiler_test::corpusLines(getenv("SWITCH_BOOT12")));
}

constexpr size_t kAsked = 2;

std::vector<std::string> clients()
{
    const std::vector<raw::CompileRequest> asked = boot12();
    std::vector<std::string> out;
    for (size_t i = asked.size() - std::min(kAsked, asked.size()); i < asked.size(); ++i)
        out.push_back(raw::encodeRequest(asked[i]));
    return out;
}

void writeLog()
{
    mkdir((dir() + "/shaders").c_str(), 0755);
    unlink(logPath().c_str());
    switch_compiler::RequestLog log;
    switch_compiler::SystemLibraries system(root());
    ASSERT_TRUE(log.open(logPath()));
    const std::vector<raw::CompileRequest> asked = boot12();
    for (size_t i = 0; i + kAsked < asked.size(); ++i) {
        switch_compiler::Prepared p;
        std::string why;
        const std::string request = raw::encodeRequest(asked[i]);
        ASSERT_TRUE(switch_compiler::prepare(request, 0, system, p, why)) << why;
        log.add(p.name, request);
    }
}

// compiler_service.mm Service as it opens and serves: its state, its pool (whose run is `compile`, then the store and the
// log as Service::run publishes), its replay of the log at the background class and the clients' requests after
struct Serving {
    switch_compiler::Store store;
    switch_compiler::RequestLog log;
    switch_compiler::SystemLibraries system{root()};
    std::vector<uint8_t> buffer = std::vector<uint8_t>(1u << 20);
    std::unique_ptr<switch_compiler::Pool> pool;

    template <typename Compile> bool open(const raw::Digest &rules, Compile compile)
    {
        std::string why;
        if (!store.open(dir() + "/shaders", 0, rules, why) || !log.open(logPath()))
            return ADD_FAILURE() << why, false;
        switch_compiler::Pool::Hooks hooks;
        hooks.run = [this, compile](switch_compiler::Work &work, const std::function<void(int32_t)> &primary) {
            Asked &asked = static_cast<Asked &>(work);
            std::string entry, failed;
            const int32_t result = compile(asked.prepared, entry, failed);
            if (!entry.empty())
                store.put(asked.prepared.name, entry);
            if (result == TegraGM20BCompileMade && !asked.request.empty())
                log.add(asked.prepared.name, asked.request);
            primary(result);
            if (asked.done)
                dispatch_semaphore_signal(asked.done);
            return result;
        };
        hooks.complete = [](const switch_compiler::Waiter &, int32_t) {};
        hooks.setBand = [](switch_compiler::Band band) {
            pthread_set_qos_class_self_np(qos_class_t(switch_compiler::qosOf(band)), 0);
        };
        hooks.raise = [](pthread_t, switch_compiler::Band) -> void * {
            return nullptr;
        };
        hooks.lower = [](void *) {};
        pool = std::make_unique<switch_compiler::Pool>(std::move(hooks));
        return true;
    }

    // As Service::submit: true where a job of its own runs it
    bool submit(switch_compiler::Prepared prepared, std::string request, switch_compiler::Band band,
                dispatch_semaphore_t done)
    {
        auto work = std::make_unique<Asked>();
        work->prepared = std::move(prepared);
        work->request = std::move(request);
        work->done = done;
        const switch_compiler::Name name = work->prepared.name;
        return pool->submit(name, std::move(work), {1, 1, band, false});
    }

    // As Service::warm: the log's requests one at a time, then the clients'
    void serve(Steps &steps)
    {
        dispatch_semaphore_t ran = dispatch_semaphore_create(0);
        log.replay([&](const std::string &request, raw::Digest &name) {
            switch_compiler::Prepared p;
            std::string why;
            if (!switch_compiler::prepare(request, 0, system, p, why))
                return false;
            name = p.name;
            if (submit(std::move(p), std::string(), switch_compiler::Band::Background, ran))
                dispatch_semaphore_wait(ran, DISPATCH_TIME_FOREVER);
            return true;
        });
        pool->drain();
        steps.step("replayed the log");
        for (const std::string &request : clients()) {
            switch_compiler::Prepared p;
            std::string why;
            if (switch_compiler::prepare(request, 0, system, p, why))
                submit(std::move(p), request, switch_compiler::Band::Interactive, nullptr);
            else
                ADD_FAILURE() << why;
        }
        pool->drain();
        steps.step("served the clients");
        std::this_thread::sleep_for(std::chrono::seconds(6));
        steps.step("at rest");
        malloc_zone_pressure_relief(nullptr, 0);
        steps.step("relieved");
    }
};

void environment()
{
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    setenv("NMB_SHADOW_LOG", "0", 0);
}

void withTheDevice()
{
    environment();
    Steps steps{"device"};
    raw::Pack pack;
    std::string why;
    ASSERT_TRUE(pack.load(packPath().c_str(), 0, why)) << why;
    std::mutex opening, caching;
    id<MTLDevice> device = nil;
    std::unique_ptr<raw::Direct> direct;
    std::map<std::string, id<MTLLibrary>> libraries;
    Serving serving;
    const auto compile = [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
        {
            std::lock_guard<std::mutex> l(opening);
            if (!device) {
                @autoreleasepool {
                    device = native_bridge_metal_open(loader().c_str(), nullptr, 0, nullptr);
                }
                direct = std::make_unique<raw::Direct>(pack);
            }
        }
        const switch_compiler::Libraries made = compiler_test::deviceLibraries(device, root());
        const switch_compiler::Libraries cached = [&](const raw::RequestLibrary &l, std::string &missed) {
            const raw::Digest digest = raw::sha256(l.bytes.data(), l.bytes.size());
            const std::string name = l.system ? l.path : std::string(digest.begin(), digest.end());
            std::lock_guard<std::mutex> g(caching);
            __strong id<MTLLibrary> &library = libraries[name];
            if (!library)
                library = made(l, missed);
            return library;
        };
        return switch_compiler::makeEntry(p, cached, *direct, 0, entry, failed);
    };
    ASSERT_TRUE(serving.open(raw::sha256("", 0), compile));
    steps.step("serving");
    serving.serve(steps);
}

void withTheEngine()
{
    environment();
    Steps steps{"context"};
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {packPath(), loader(), root()}, why)) << why;
    Serving serving;
    ASSERT_TRUE(
        serving.open(engine.rules(), [&](const switch_compiler::Prepared &p, std::string &entry, std::string &failed) {
            return engine.compile(p, entry, failed);
        }));
    steps.step("serving");
    serving.serve(steps);
}

}  // namespace

TEST(CompilerFootprint, AtLaunch)
{
    switch_ios::test::ExpectPassesInFreshProcess([] {
        Steps steps{"launch"};
        steps.owners = {};
        steps.step("launch");
    });
    show("launch");
}

TEST(CompilerFootprint, ServingBoot12WithTheM2VDevice)
{
    writeLog();
    switch_ios::test::ExpectPassesInFreshProcess(withTheDevice);
    show("device");
}

TEST(CompilerFootprint, ServingBoot12WithTheCompileContext)
{
    writeLog();
    switch_ios::test::ExpectPassesInFreshProcess(withTheEngine);
    show("context");
}
