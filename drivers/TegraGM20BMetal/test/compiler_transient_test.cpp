// SPDX-License-Identifier: MIT
// What one compile of the system shader compiler's heaviest states costs at its peak, each state a process of its own over
// the mock GM20B, as the compiler makes it (compiler_engine.h, the first compile of the process): RenderBox's uber states
// and Core Animation's generic fixed-function state with every image input (PBGRAXm_A2Xghfc). A sampler beside the
// compile reads the heap in use and the compiling thread's stack every millisecond: each state's line gives its time, its
// peak heap and phys_footprint, the function the compile was in at the peak, and the most heap the compile held while
// each part of it ran (m2v's translation, NIR, NAK, NVK, the raw record). Run by hand with MallocStackLogging=1 and
// COMPILER_TRANSIENT_SITES=<dir>, a state's run also leaves malloc_history's live allocations there each time the heap
// passes a new high by a quarter; COMPILER_TRANSIENT_ENTRIES=<dir> keeps each state's entry there, to compare two builds'
// records byte for byte.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include "NativeBridgeMetalFrontend.h"
#include "bazel/testing/fresh_process.h"
#include "compiler_requests.h"
#include "../compiler/compiler_engine.h"

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

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

// The corpus's RenderBox uber states and Core Animation's generic state with every image input
std::vector<std::string> heavy()
{
    std::vector<std::string> out;
    for (const std::string &line : compiler_test::corpusLines(getenv("SWITCH_CORPUS")))
        if (line.find("| f uber_fragment lib=/System/Library/PrivateFrameworks/RenderBox") != std::string::npos ||
            line.find(" label=com.apple.coreanimation.PBGRAXm_A2Xghfc ") != std::string::npos)
            out.push_back(line);
    return out;
}

uint64_t heapInUse()
{
    malloc_statistics_t heap = {};
    malloc_zone_statistics(nullptr, &heap);
    return heap.size_in_use;
}

// The part of the compile a symbol is in, "" for none
const char *partOf(const char *symbol)
{
    static const std::pair<const char *, const char *> parts[] = {
        {"3nak", "NAK"},        {"nak_", "NAK"},         {"nir_", "NIR"},
        {"spirv", "NIR"},       {"metal2vulkan", "m2v"}, {"nvk_", "NVK"},
        {"vk_pipeline", "NVK"}, {"N3raw", "raw"},        {"native_metal_bridge", "bridge"}};
    for (const auto &[needle, part] : parts)
        if (strstr(symbol, needle))
            return part;
    return "";
}

// The compiling thread's frames, innermost first, read by walking its frame pointers while it is suspended
std::vector<const void *> framesOf(thread_act_t thread)
{
    constexpr size_t kFrames = 96;
    std::vector<const void *> frames;
    // nothing allocates while the thread is suspended: it may hold the heap's lock
    frames.reserve(kFrames + 2);
    if (thread_suspend(thread) != KERN_SUCCESS)
        return frames;
    arm_thread_state64_t state = {};
    mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(thread, ARM_THREAD_STATE64, reinterpret_cast<thread_state_t>(&state), &count) ==
        KERN_SUCCESS) {
        frames.push_back(reinterpret_cast<const void *>(arm_thread_state64_get_pc(state)));
        frames.push_back(reinterpret_cast<const void *>(arm_thread_state64_get_lr(state)));
        auto fp = reinterpret_cast<const uintptr_t *>(arm_thread_state64_get_fp(state));
        for (size_t i = 0; fp && i < kFrames && (uintptr_t(fp) & 7) == 0; ++i) {
            frames.push_back(reinterpret_cast<const void *>(fp[1] & 0x0000ffffffffffffull));
            const auto next = reinterpret_cast<const uintptr_t *>(fp[0]);
            if (next <= fp)
                break;
            fp = next;
        }
    }
    thread_resume(thread);
    return frames;
}

struct Sampler {
    thread_act_t compiling = mach_thread_self();
    std::atomic<bool> done{false};
    uint64_t base = heapInUse(), peak = 0, nextSites = 0;
    std::vector<const void *> atPeak;
    std::map<std::string, uint64_t> partPeak;
    std::thread thread;

    void start(const std::string &name)
    {
        nextSites = base + (base >> 2);
        thread = std::thread([this, name] {
            while (!done) {
                const uint64_t now = heapInUse();
                const std::vector<const void *> frames = framesOf(compiling);
                std::string part;
                for (const void *pc : frames) {
                    Dl_info info = {};
                    if (dladdr(pc, &info) && info.dli_sname && *partOf(info.dli_sname)) {
                        part = partOf(info.dli_sname);
                        break;
                    }
                }
                uint64_t &held = partPeak[part.empty() ? "other" : part];
                held = std::max(held, now - std::min(now, base));
                if (now > peak) {
                    peak = now;
                    atPeak = frames;
                }
                if (const char *sites = getenv("COMPILER_TRANSIENT_SITES"); sites && now > nextSites) {
                    nextSites = now + (now >> 2);
                    const std::string out = std::string(sites) + "/" + name + "-" + std::to_string(now >> 20) + "MB";
                    system(("/usr/bin/malloc_history " + std::to_string(getpid()) + " -allBySize > '" + out + "' 2>&1")
                               .c_str());
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
    }

    std::string stop()
    {
        done = true;
        thread.join();
        std::string where;
        for (const void *pc : atPeak) {
            Dl_info info = {};
            if (dladdr(pc, &info) && info.dli_sname && *partOf(info.dli_sname)) {
                where = info.dli_sname;
                break;
            }
        }
        std::string parts;
        for (const auto &[part, held] : partPeak)
            parts += " " + part + "=" + std::to_string(held >> 20) + "MB";
        return "peak heap " + std::to_string((peak - std::min(peak, base)) >> 20) + " MB over " +
               std::to_string(base >> 20) + " MB, in " + where.substr(0, 120) + ";" + parts;
    }
};

void compileOne(size_t index)
{
    setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
    setenv("NVK_DARWIN_SERVICE", "AGXAccelerator", 0);
    const std::string line = heavy()[index];
    compiler_test::Requests made(root());
    raw::CompileRequest r;
    std::string why;
    ASSERT_TRUE(made.render(line, r, why)) << why;
    switch_compiler::SystemLibraries system(root());
    switch_compiler::Prepared p;
    ASSERT_TRUE(switch_compiler::prepare(raw::encodeRequest(r), 0, system, p, why)) << why;
    switch_compiler::Engine engine;
    ASSERT_TRUE(engine.open(0, {compiler_test::emptyPack(dir()), loader(), root()}, why)) << why;
    const std::string label =
        line.substr(line.find("label=") + 6, line.find(' ', line.find("label=")) - line.find("label=") - 6);
    Sampler sampler;
    sampler.start(label + "-" + std::to_string(index));
    const auto began = std::chrono::steady_clock::now();
    std::string entry;
    const int32_t result = engine.compile(p, entry, why);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
    const std::string peaks = sampler.stop();
    task_vm_info_data_t vm = {};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&vm), &count);
    if (const char *entries = getenv("COMPILER_TRANSIENT_ENTRIES"))
        std::ofstream(std::string(entries) + "/" + std::to_string(index) + ".entry", std::ios::binary) << entry;
    std::ofstream(dir() + "/transient.txt", std::ios::app)
        << label << " " << index << ": result " << result << " in " << ms << " ms, entry " << entry.size()
        << " bytes, footprint peak " << (vm.ledger_phys_footprint_peak >> 20) << " MB; " << peaks << "\n";
}

}  // namespace

TEST(CompilerTransient, EachHeavyStateInAProcessOfItsOwn)
{
    const size_t states = heavy().size();
    if (!states || root().empty())
        GTEST_SKIP() << "this crawl names no RenderBox uber state";
    for (size_t i = 0; i < states; ++i)
        switch_ios::test::ExpectPassesInFreshProcess([i] {
            compileOne(i);
        });
    std::ifstream in(dir() + "/transient.txt");
    for (std::string l; std::getline(in, l);)
        printf("%s\n", l.c_str());
}
