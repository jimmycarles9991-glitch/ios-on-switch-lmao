// Render pipeline compilation uses bounded workers and per-state completion observers. The native command scheduler
// treats a pending pipeline as a dependency; it never submits a pass with missing draws, and has the one a committed
// command buffer waits for made at once at its class (urge). Direct bridge callers retain
// synchronous set semantics, with an explicit error on failure or timeout. Lazy compatibility states start only on need.
#pragma once
#import <Foundation/Foundation.h>
#import <TargetConditionals.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <pthread/qos.h>
#include <os/log.h>
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thread>
#include <vector>
#include <time.h>

namespace nbasync {

inline uint64_t usNow()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

struct Policy {
    bool on = true;   // sync API creations in this process are made on a worker
    bool api = true;  // the completion-handler API is made on a worker
    unsigned threads = 2;
};

inline uint64_t envNumber(const char *name, uint64_t otherwise)
{
    const char *v = getenv(name);
    if (!v || !*v)
        return otherwise;
    char *end = nullptr;
    const unsigned long long n = strtoull(v, &end, 10);
    return end && *end == 0 ? (uint64_t)n : otherwise;
}

inline const Policy &policy()
{
    static Policy p;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const bool server = strcmp(getprogname(), "backboardd") == 0;
#if TARGET_OS_IPHONE
        const uint64_t device = 1;
#else
        const uint64_t device = 0;  // the host's tools (precompile.mm's seed, hostsim) count on each state made at its call
#endif
        p.on = envNumber("NMB_ASYNC_PSO", device) != 0;
        p.threads = server ? 3 : 2;
        p.api = envNumber("NMB_ASYNC_PSO_API", device) != 0;
        p.threads =
            (unsigned)std::max<uint64_t>(1, std::min<uint64_t>(4, envNumber("NMB_ASYNC_PSO_THREADS", p.threads)));
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
        if (p.on || p.api)
            NSLog(@"native-metal: async pipelines: sync API %s, completion API %s, %u workers", p.on ? "on" : "off",
                  p.api ? "on" : "off", p.threads);
#endif
    });
    return p;
}

// The counters behind the rates line's apso= part, taken (and cleared) by appendRates.
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
struct Counters {
    std::atomic<uint64_t> started{0}, made{0}, failed{0};  // creations handed to a worker, made, refused
    std::atomic<uint64_t> pending{0}, caught{0};  // sets that found a state being made, and of them made in the wait
    std::atomic<uint64_t> skippedSets{0}, skippedDraws{0};  // sets that gave up (or found it failed), draws skipped
    std::atomic<uint64_t> offWaits{0}, offWaitUs{0};        // sets in other passes that waited it out, and for how long
    std::atomic<uint64_t> readyN{0}, readyUs{0}, readyMaxUs{0};  // asked to made (worker creations)
    std::atomic<uint64_t> waitedUs{0}, waitedMaxUs{0};           // bounded waits' time on the asking thread
    std::atomic<uint64_t> redraws{0};
    std::atomic<uint64_t> lazy{0}, lazySet{0};  // lazy creations handed out, and of them asked for by a set
};

inline Counters &counters()
{
    static Counters c;
    return c;
}

inline void raiseMax(std::atomic<uint64_t> &max, uint64_t v)
{
    uint64_t seen = max.load(std::memory_order_relaxed);
    while (v > seen && !max.compare_exchange_weak(seen, v, std::memory_order_relaxed)) {
    }
}

#endif
// One creation in flight. `work` makes it with the synchronous path (on a worker, or on a set that waits it out) and returns
// the state it made (an NBRenderPipeline that owns its handle), or nil with `failure`.
struct Making {
    enum State {
        Queued,
        Running,
        Made,
        Failed
    };

    std::mutex lock;
    std::condition_variable finished;
    State state = Queued;
    bool lazy = false;  // startLazy's, not asked for by a set yet
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    uint64_t asked = 0;
#endif
    pthread_t runner = nullptr;  // the thread making it, once one does
    qos_class_t runnerClass = QOS_CLASS_UNSPECIFIED;
    pthread_override_t urged = nullptr;  // urge()'s class on the runner, until the state is made
    __strong id result = nil;            // the state the synchronous path made
    std::function<id(NSString **failure)> work;
    std::string label;
    std::vector<std::function<void()>> observers;

    bool done() const
    {
        return state == Made || state == Failed;
    }
};

// Run `m`'s creation here when nothing took it yet; false when another thread runs or ran it.
inline bool run(const std::shared_ptr<Making> &m)
{
    std::function<id(NSString **)> work;
    {
        std::lock_guard<std::mutex> g(m->lock);
        if (m->state != Making::Queued)
            return false;
        m->state = Making::Running;
        m->runner = pthread_self();
        m->runnerClass = qos_class_self();
        work.swap(m->work);
    }
    id made = nil;
    NSString *failure = nil;
    @autoreleasepool {
        made = work(&failure);
        if (!made)
            NSLog(@"native-metal: async pipeline %s refused: %@", m->label.c_str(), failure ?: @"?");
    }
    std::vector<std::function<void()>> observers;
    pthread_override_t urged;
    {
        std::lock_guard<std::mutex> g(m->lock);
        m->result = made;
        m->state = made ? Making::Made : Making::Failed;
        observers.swap(m->observers);
        urged = m->urged;
        m->urged = nullptr;
    }
    if (urged)
        pthread_override_qos_class_end_np(urged);
    m->finished.notify_all();
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    Counters &c = counters();
    (made ? c.made : c.failed)++;
    const uint64_t took = usNow() - m->asked;
    c.readyN++;
    c.readyUs += took;
    raiseMax(c.readyMaxUs, took);
#endif
    for (const auto &observer : observers)
        observer();
    return true;
}

// The workers: policy().threads threads at the user-initiated class, started as work arrives.
class Workers {
    std::mutex lock;
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    unsigned threads = 0, idle = 0;

    void serve()
    {
        pthread_setname_np("native-metal-async-pso");
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
        std::unique_lock<std::mutex> g(lock);
        for (;;) {
            while (jobs.empty()) {
                idle++;
                wake.wait(g);
                idle--;
            }
            std::function<void()> job = std::move(jobs.front());
            jobs.pop_front();
            g.unlock();
            job();
            g.lock();
        }
    }

public:
    void submit(std::function<void()> job)
    {
        std::lock_guard<std::mutex> g(lock);
        jobs.push_back(std::move(job));
        // A worker counts as idle until it wakes, so an earlier job's wakeup may be addressed to it already: each idle worker
        // takes one queued job, and a job beyond those starts a worker of its own
        if (jobs.size() > idle && threads < policy().threads) {
            threads++;
            std::thread([this] {
                serve();
            }).detach();
        } else {
            wake.notify_one();
        }
    }
};

inline Workers &workers()
{
    static Workers *w = new Workers();  // never destroyed: workers outlive static destruction
    return *w;
}

// Hand `m` to the workers.
inline void start(const std::shared_ptr<Making> &m)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    m->asked = usNow();
    counters().started++;
#endif
    workers().submit([m] {
        run(m);
    });
}

// Hand out `m` without making it: its first set does (wanted()).
inline void startLazy(const std::shared_ptr<Making> &m)
{
    m->lazy = true;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    counters().started++;
    counters().lazy++;
#endif
}

// A set of `m` (under m->lock): a lazy creation is asked for from now, and a pass that skips hands it to the workers.
inline void wanted(const std::shared_ptr<Making> &m, bool skips)
{
    if (!m->lazy)
        return;
    m->lazy = false;
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    m->asked = usNow();
    counters().lazySet++;
#endif
    if (skips)
        workers().submit([m] {
            run(m);
        });
}

// The thread making `m` (m->lock held) at least at the calling thread's class: a render thread (no class: Core Animation's
// is a fixed priority) lends the user-interactive class. Null where it runs there already.
inline pthread_override_t lend(const Making &m)
{
    if (m.state != Making::Running || !m.runner || pthread_equal(m.runner, pthread_self()))
        return nullptr;
    qos_class_t mine = qos_class_self();
    if (mine == QOS_CLASS_UNSPECIFIED)
        mine = QOS_CLASS_USER_INTERACTIVE;
    const qos_class_t runner = m.runnerClass == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : m.runnerClass;
    return mine > runner ? pthread_override_qos_class_start_np(m.runner, mine, 0) : nullptr;
}

// While the calling thread waits for `m` (`lock` held, as `m->lock`'s guard), the thread making it runs at its class.
struct Lend {
    pthread_override_t override = nullptr;

    Lend(const Making &m) : override(lend(m))
    {}

    ~Lend()
    {
        if (override)
            pthread_override_qos_class_end_np(override);
    }
};

// Nonblocking readiness: 1 ready, 0 pending, -1 failed. Register and inspect under the same lock so completion cannot
// race past an observer. Observers run after publication and without the pipeline lock; they must not retain the job.
inline int observe(const std::shared_ptr<Making> &m, std::function<void()> observer = {})
{
    std::lock_guard<std::mutex> g(m->lock);
    if (m->state == Making::Made)
        return 1;
    if (m->state == Making::Failed)
        return -1;
    wanted(m, true);
    if (observer)
        m->observers.push_back(std::move(observer));
    return 0;
}

// observe() for a caller that has urge() make the creation: a lazy one is not handed to the workers as well, where an idle
// worker would take it first, at its own class
inline int claim(const std::shared_ptr<Making> &m, std::function<void()> observer = {})
{
    std::lock_guard<std::mutex> g(m->lock);
    if (m->state == Making::Made)
        return 1;
    if (m->state == Making::Failed)
        return -1;
    wanted(m, false);
    if (observer)
        m->observers.push_back(std::move(observer));
    return 0;
}

// A creation a committed command buffer waits for, wanted now at the calling thread's class without waiting for it: made here
// when no worker has started it (ahead of every creation queued before it), else the worker making it runs at this class
// until it is made. 1 made, 0 being made elsewhere, -1 failed.
inline int urge(const std::shared_ptr<Making> &m)
{
    {
        std::lock_guard<std::mutex> g(m->lock);
        if (m->done())
            return m->state == Making::Made ? 1 : -1;
        wanted(m, false);
    }
    if (!run(m)) {
        std::lock_guard<std::mutex> g(m->lock);
        if (m->done())
            return m->state == Making::Made ? 1 : -1;
        if (!m->urged)
            m->urged = lend(*m);
        return 0;
    }
    std::lock_guard<std::mutex> g(m->lock);
    return m->state == Making::Made ? 1 : -1;
}

// For direct bridge clients without the native job scheduler. A failed/expired compile fails their command buffer;
// no timeout turns a pass with missing draws into a successful command buffer.
inline bool ready(const std::shared_ptr<Making> &m)
{
    {
        std::lock_guard<std::mutex> g(m->lock);
        if (m->done())
            return m->state == Making::Made;
        wanted(m, false);
    }
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    Counters &c = counters();
    c.pending++;
    const uint64_t began = usNow();
#endif
    run(m);
    std::unique_lock<std::mutex> g(m->lock);
    Lend lend(*m);
    const bool completed = m->finished.wait_for(g, std::chrono::seconds(60), [&] {
        return m->done();
    });
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    c.offWaits++;
    c.offWaitUs += usNow() - began;
#endif
    return completed && m->state == Making::Made;
}

// The rates line's part (taken: each window's own): apso=<handed>/<made>/<refused> set=<found being made>/<made in the
// bounded wait> wait=<total>/<max>ms skip=<sets given up>/<draws skipped> off=<waited out>/<ms> rdy=<mean>/<max>ms
// redraw=<n> lazy=<lazy creations handed out>/<lazy creations a set asked for>. Nothing while no creation was handed and
// no set found one being made.
inline size_t appendRates(char *out, size_t size)
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    Counters &c = counters();
    const uint64_t started = c.started.exchange(0), made = c.made.exchange(0), failed = c.failed.exchange(0);
    const uint64_t pending = c.pending.exchange(0), caught = c.caught.exchange(0), sets = c.skippedSets.exchange(0);
    const uint64_t draws = c.skippedDraws.exchange(0), off = c.offWaits.exchange(0), offUs = c.offWaitUs.exchange(0);
    const uint64_t readyN = c.readyN.exchange(0), readyUs = c.readyUs.exchange(0), readyMax = c.readyMaxUs.exchange(0);
    const uint64_t waited = c.waitedUs.exchange(0), waitedMax = c.waitedMaxUs.exchange(0),
                   redraws = c.redraws.exchange(0), lazy = c.lazy.exchange(0), lazySet = c.lazySet.exchange(0);
    if (!size || (!started && !pending && !draws && !made && !failed))
        return 0;
    const int n =
        snprintf(out, size,
                 " apso=%llu/%llu/%llu set=%llu/%llu wait=%llu/%llums skip=%llu/%llu off=%llu/%llums "
                 "rdy=%llu/%llums redraw=%llu lazy=%llu/%llu",
                 (unsigned long long)started, (unsigned long long)made, (unsigned long long)failed,
                 (unsigned long long)pending, (unsigned long long)caught, (unsigned long long)(waited / 1000),
                 (unsigned long long)(waitedMax / 1000), (unsigned long long)sets, (unsigned long long)draws,
                 (unsigned long long)off, (unsigned long long)(offUs / 1000),
                 (unsigned long long)(readyN ? readyUs / readyN / 1000 : 0), (unsigned long long)(readyMax / 1000),
                 (unsigned long long)redraws, (unsigned long long)lazy, (unsigned long long)lazySet);
    return n > 0 ? std::min<size_t>((size_t)n, size - 1) : 0;
#else
    (void)out;
    (void)size;
    return 0;
#endif
}

}  // namespace nbasync
