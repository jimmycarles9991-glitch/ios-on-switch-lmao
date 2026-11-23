// SPDX-License-Identifier: MIT
// What the system shader compiler holds as it serves boot 12's requests (compiler_boot12.log), each run a process of its
// own, as compiler_service.mm opens and serves: the pack, the store, the request log an earlier boot
// left (boot 12's 13 replayed requests), the take buffer and the pool's workers; the log replayed at the background
// class; the 2 requests clients then asked; the compiler at rest (its libraries let go: compiler_libraries.h), and the
// heap relieved (malloc_zone_pressure_relief). Each step's line: the heap in use and allocated (the device's footprint
// line's figures), phys_footprint and its peak, and what grew, by image and VM
// tag (footprint_breakdown.h). Run by hand: SWITCH_HWTEST_METALLIB names the HW Test app's library (its states are left
// out without it), SWITCH_PACK a raw pack, and with MallocStackLogging=1, COMPILER_FOOTPRINT_SITES=<dir> keeps
// malloc_history's allocations by size and vmmap's summary at each step there.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <malloc/malloc.h>
#include <pthread/qos.h>
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "footprint_breakdown.h"
#include "../compiler/compiler_engine.h"
#include "../compiler/compiler_log.h"
#include "../compiler/compiler_pool.h"
#include "../compiler/compiler_store.h"
#include "../pack.h"

#include <sys/stat.h>
#include <unistd.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <thread>
#include "TegraGM20BCompilerABI.h"

namespace {

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
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
std::vector<gm20b::CompileRequest> boot12()
{
    const char *hwtest = getenv("SWITCH_HWTEST_METALLIB");
    const bool withHwtest = hwtest && *hwtest;
    std::vector<std::string> lines = compiler_test::corpusLines(getenv("SWITCH_BOOT12"));
    if (!withHwtest)
        lines.erase(std::remove_if(lines.begin(), lines.end(),
                                   [](const std::string &line) {
                                       return line.find("lib=/Applications/SwitchHWTest.app/default.metallib") !=
                                              std::string::npos;
                                   }),
                    lines.end());
    EXPECT_EQ(lines.size(), withHwtest ? 15u : 5u);
    compiler_test::Requests made(root(), {{"/Applications/SwitchHWTest.app/default.metallib", hwtest ?: ""}});
    std::vector<gm20b::CompileRequest> asked = made.of(lines);
    EXPECT_EQ(asked.size(), lines.size());
    return asked;
}

constexpr size_t kAsked = 2;

std::vector<std::string> clients(size_t &total)
{
    const std::vector<gm20b::CompileRequest> asked = boot12();
    total = asked.size();
    std::vector<std::string> out;
    for (size_t i = asked.size() - std::min(kAsked, asked.size()); i < asked.size(); ++i)
        out.push_back(gm20b::encodeRequest(asked[i]));
    return out;
}

void writeLog()
{
    mkdir((dir() + "/shaders").c_str(), 0755);
    unlink(logPath().c_str());
    switch_compiler::RequestLog log;
    switch_compiler::SystemLibraries system(root());
    ASSERT_TRUE(log.open(logPath()));
    const std::vector<gm20b::CompileRequest> asked = boot12();
    ASSERT_GT(asked.size(), kAsked);
    for (size_t i = 0; i + kAsked < asked.size(); ++i) {
        switch_compiler::Prepared p;
        std::string why;
        const std::string request = gm20b::encodeRequest(asked[i]);
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

    template <typename Compile> bool open(const gm20b::Digest &rules, Compile compile)
    {
        std::string why;
        if (!store.open(dir() + "/shaders", 0, rules, why) || !log.open(logPath()))
            return ADD_FAILURE() << why, false;
        switch_compiler::Pool::Hooks hooks;
        hooks.run = [this, compile](switch_compiler::Work &work, const std::function<void(int32_t)> &primary) {
            Asked &asked = static_cast<Asked &>(work);
            std::string entry, failed;
            const int32_t result = compile(asked.prepared, entry, failed);
            EXPECT_EQ(result, TegraGM20BCompileMade) << asked.prepared.packKey << ": " << failed;
            EXPECT_FALSE(entry.empty()) << asked.prepared.packKey;
            if (!entry.empty())
                EXPECT_EQ(store.put(asked.prepared.name, entry), switch_compiler::Store::Put::Added);
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
        size_t replayed = 0;
        log.replay([&](const std::string &request, gm20b::Digest &name) {
            switch_compiler::Prepared p;
            std::string why;
            if (!switch_compiler::prepare(request, 0, system, p, why)) {
                ADD_FAILURE() << why;
                return false;
            }
            name = p.name;
            ++replayed;
            if (submit(std::move(p), std::string(), switch_compiler::Band::Background, ran))
                dispatch_semaphore_wait(ran, DISPATCH_TIME_FOREVER);
            return true;
        });
        pool->drain();
        steps.step("replayed the log");
        size_t total = 0;
        for (const std::string &request : clients(total)) {
            switch_compiler::Prepared p;
            std::string why;
            if (switch_compiler::prepare(request, 0, system, p, why))
                submit(std::move(p), request, switch_compiler::Band::Interactive, nullptr);
            else
                ADD_FAILURE() << why;
        }
        pool->drain();
        ASSERT_GT(total, kAsked);
        EXPECT_EQ(replayed, total - kAsked);
        EXPECT_EQ(pool->stats().jobs, total);
        EXPECT_EQ(store.entries(), total);
        steps.step("served the clients");
        std::this_thread::sleep_for(std::chrono::seconds(6));
        steps.step("at rest");
        malloc_zone_pressure_relief(nullptr, 0);
        steps.step("relieved");
    }
};

void withTheEngine()
{
    Steps steps{"context"};
    switch_compiler::Engine engine;
    std::string why;
    ASSERT_TRUE(engine.open(0, {packPath(), root(), {}}, why)) << why;
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

TEST(CompilerFootprint, ServingBoot12WithTheCompileContext)
{
    writeLog();
    switch_ios::test::ExpectPassesInFreshProcess(withTheEngine);
    show("context");
}
