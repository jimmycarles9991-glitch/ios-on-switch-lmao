// SPDX-License-Identifier: MIT
// A test's watchdog: where its phases make no progress for `limit`, or the test runner's SIGTERM comes (a Bazel
// timeout), it prints the phase, the wall and process CPU time since the last progress (a starved executor still runs, a
// hang does not), what `describe` says of the code under test, and every thread's frames, then aborts. A remote test
// cannot be sampled: this is its sample. Each thread is suspended only while its registers and frame records are read
// into storage made beforehand, so a thread suspended inside malloc or dyld cannot stall the dump.
#pragma once
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>

namespace stall_watchdog {

inline int &termPipe()
{
    static int fds[2] = {-1, -1};
    return fds[1];
}

inline double processCpuSeconds()
{
    rusage use = {};
    getrusage(RUSAGE_SELF, &use);
    return double(use.ru_utime.tv_sec + use.ru_stime.tv_sec) +
           double(use.ru_utime.tv_usec + use.ru_stime.tv_usec) / 1e6;
}

inline bool readWord(uintptr_t at, uintptr_t &out)
{
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), at, sizeof out, vm_address_t(&out), &got) == KERN_SUCCESS &&
           got == sizeof out;
}

// Every other thread's name, CPU time and frames
inline void dumpThreads()
{
    constexpr unsigned kThreads = 128, kFrames = 48;
    static uintptr_t pcs[kThreads][kFrames];
    static unsigned depth[kThreads];
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
        return;
    const mach_port_t self = mach_thread_self();
    const unsigned n = count < kThreads ? count : kThreads;
    for (unsigned t = 0; t < n; t++) {
        depth[t] = 0;
        if (threads[t] == self || thread_suspend(threads[t]) != KERN_SUCCESS)
            continue;
        arm_thread_state64_t state = {};
        mach_msg_type_number_t words = ARM_THREAD_STATE64_COUNT;
        if (thread_get_state(threads[t], ARM_THREAD_STATE64, thread_state_t(&state), &words) == KERN_SUCCESS) {
            pcs[t][depth[t]++] = uintptr_t(arm_thread_state64_get_pc(state)) & 0x00007fffffffffffull;
            pcs[t][depth[t]++] = uintptr_t(arm_thread_state64_get_lr(state)) & 0x00007fffffffffffull;
            uintptr_t fp = uintptr_t(arm_thread_state64_get_fp(state));
            uintptr_t next = 0, lr = 0;
            while (depth[t] < kFrames && fp && !(fp & 7) && readWord(fp, next) && readWord(fp + 8, lr) && lr) {
                pcs[t][depth[t]++] = lr & 0x00007fffffffffffull;
                if (next <= fp)
                    break;
                fp = next;
            }
        }
        thread_resume(threads[t]);
    }
    for (unsigned t = 0; t < n; t++) {
        char name[64] = "";
        if (pthread_t p = pthread_from_mach_thread_np(threads[t]))
            pthread_getname_np(p, name, sizeof name);
        thread_basic_info_data_t info = {};
        mach_msg_type_number_t infoCount = THREAD_BASIC_INFO_COUNT;
        thread_info(threads[t], THREAD_BASIC_INFO, thread_info_t(&info), &infoCount);
        fprintf(stderr, "watchdog: thread %u%s \"%s\" cpu %d.%06ds run_state %d\n", t,
                threads[t] == self ? " (the watchdog)" : "", name, info.user_time.seconds + info.system_time.seconds,
                info.user_time.microseconds + info.system_time.microseconds, info.run_state);
        for (unsigned k = 0; k < depth[t]; k++) {
            Dl_info d = {};
            dladdr(reinterpret_cast<void *>(pcs[t][k]), &d);
            fprintf(stderr, "  %2u %s+0x%lx (%s)\n", k, d.dli_sname ? d.dli_sname : "?",
                    (unsigned long)(pcs[t][k] - uintptr_t(d.dli_sname ? d.dli_saddr : d.dli_fbase)),
                    d.dli_fname ? (strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname) : "?");
        }
    }
    for (unsigned t = 0; t < count; t++)
        mach_port_deallocate(mach_task_self(), threads[t]);
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), vm_address_t(threads), count * sizeof(thread_act_t));
}

class Watchdog {
public:
    Watchdog(std::chrono::seconds limit, std::function<void(FILE *)> describe)
        : limit_(limit), describe_(std::move(describe))
    {
        static int fds[2] = {-1, -1};
        if (fds[0] < 0 && pipe(fds) == 0) {
            termPipe() = fds[1];
            signal(SIGTERM, [](int) {
                const char c = 't';
                (void)!write(termPipe(), &c, 1);
            });
        }
        termRead_ = fds[0];
        progress("start");
        thread_ = std::thread([this] {
            watch();
        });
    }

    ~Watchdog()
    {
        stopping_.store(true);
        thread_.join();
    }

    void progress(const char *phase)
    {
        phase_.store(phase, std::memory_order_relaxed);
        steps_.fetch_add(1, std::memory_order_relaxed);
        at_.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    }

private:
    void watch()
    {
        double cpuAt = processCpuSeconds();
        uint64_t seen = steps_.load();
        while (!stopping_.load()) {
            pollfd p = {termRead_, POLLIN, 0};
            const bool term = poll(&p, 1, 250) > 0;
            const uint64_t steps = steps_.load(std::memory_order_relaxed);
            if (steps != seen) {
                seen = steps;
                cpuAt = processCpuSeconds();
                continue;
            }
            const auto since = std::chrono::steady_clock::now() -
                               std::chrono::steady_clock::time_point(
                                   std::chrono::steady_clock::duration(at_.load(std::memory_order_relaxed)));
            if (!term && since < limit_)
                continue;
            fprintf(stderr, "watchdog: %s in phase \"%s\" after %llu steps: %.1f s wall, %.2f s process CPU since\n",
                    term ? "SIGTERM" : "no progress", phase_.load(), (unsigned long long)steps,
                    std::chrono::duration<double>(since).count(), processCpuSeconds() - cpuAt);
            if (describe_)
                describe_(stderr);
            dumpThreads();
            fflush(stderr);
            abort();
        }
    }

    std::chrono::seconds limit_;
    std::function<void(FILE *)> describe_;
    std::atomic<const char *> phase_{""};
    std::atomic<uint64_t> steps_{0};
    std::atomic<std::chrono::steady_clock::rep> at_{0};
    std::atomic<bool> stopping_{false};
    int termRead_ = -1;
    std::thread thread_;
};

}  // namespace stall_watchdog
