// Render pipeline states made off the asking thread (NativeBridgeMetalFrontend.mm includes this once).
//
// Core Animation makes a pipeline state the first time a frame needs it and draws with it at once, on its render thread. A
// state the shader cache misses costs that thread 100-330 ms (translation, then NAK's compile at creation); a hit 4-16 ms.
// A creation instead runs on a worker thread here and the call returns at once with a state that is still being made
// ([NBRenderPipeline making]); its first set decides:
//
// - **Set.** In a pass that skips (below), an encoder that sets a state still being made waits for it up to a bound
//   (NMB_ASYNC_PSO_WAIT_US, 4 ms), within a budget of waits per frame on that thread (NMB_ASYNC_PSO_FRAME_US, 4 ms; a frame
//   is 16.7 ms of that thread's time). Made by then, it is set. Otherwise the encoder sets nothing and skips every draw
//   until its next set (the m2v encoder keeps its earlier state bound, so a draw must never reach it): the pass itself
//   (its attachments, loads, stores, barriers) records unchanged, only those draws are missing.
// - **Which passes skip.** In the render server, only a pass whose first color attachment is an IOSurface of at least
//   NMB_ASYNC_PSO_MIN_PIXELS (half a 720p frame): the display's composite, which Core Animation draws again. Any other pass
//   (offscreen: a blur chain's steps, a rasterized or masked layer, filters) holds output a later frame reads without
//   drawing it again, so its set waits until the state is made (counted: off=). In every other process no pass skips (an
//   app's backing store is drawn once): a set waits until the state is made, so a run of creations before their first
//   draws (an app's launch) is made side by side on the workers, and a creation drawn at once costs as much as a
//   synchronous one.
// - **Drawn again.** Core Animation redraws only what changed, so a skipped draw could stay missing. Once a state a draw
//   skipped for is made, the server's displays are invalidated whole and an update is scheduled, through
//   -[CAWindowServerDisplay setInvertsColors:] with the value it holds (QuartzCore 21H461: IOMFBDisplay::set_inverse_color
//   returns at once on an unchanged value; the method then calls Server::invalidate with the display's shape and
//   Server::set_next_update), on the main queue, at most once per 16 ms (NMB_ASYNC_PSO_REDRAW=0: never).
// - **Failures.** A creation refused on the worker leaves its state failed: every draw with it is skipped, as a nil state's
//   would be missing (counted, logged once per state).
// - **Workers.** NMB_ASYNC_PSO_THREADS (3 in the render server, 2 elsewhere) threads at the user-initiated class, below the
//   render threads. A set that waits without a bound runs a creation no worker took yet itself; a set that waits for a
//   worker's creation lends that worker its own class meanwhile (a QoS override).
// - **Lazy creations** (startLazy: TegraGM20BMetal's m2v twins of the states its raw path draws, which only a command
//   buffer that falls back to m2v sets) are made at their first set: a pass that skips hands it to the workers, one that
//   waits makes it on the setting thread.
// - **The asynchronous API** (newRenderPipelineStateWithDescriptor:completionHandler: and the compute forms) makes the state
//   on a global queue of the caller's class (not these workers) and calls back there, in every process (NMB_ASYNC_PSO_API=0:
//   on the caller's thread).
//
// NMB_ASYNC_PSO and NMB_ASYNC_PSO_API: 1 on, 0 every creation on the asking thread; both on by default on the
// device, off on the host (its tools make states and exit: precompile.mm's seed). NMB_ASYNC_PSO_ONSCREEN: surfaces (the render
// server's default), none (every other process's: never skip), all (every pass skips: host tests). Counters: the rates
// line's apso= part (appendRates).
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
#include <time.h>

namespace nbasync {

inline uint64_t usNow()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

struct Policy {
    bool on = true;      // sync API creations in this process are made on a worker
    bool api = true;     // the completion-handler API is made on a worker
    bool redraw = true;  // invalidate the displays once a skipped-for state is made
    int onscreen = 0;    // 0: IOSurface passes of minPixels and more; 1: every pass; 2: none
    uint64_t waitUs = 4000, frameUs = 4000, minPixels = 1280 * 720 / 2;
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
        p.onscreen = server ? 0 : 2;
        p.threads = server ? 3 : 2;
        p.api = envNumber("NMB_ASYNC_PSO_API", device) != 0;
        p.redraw = envNumber("NMB_ASYNC_PSO_REDRAW", 1) != 0;
        p.waitUs = envNumber("NMB_ASYNC_PSO_WAIT_US", p.waitUs);
        p.frameUs = envNumber("NMB_ASYNC_PSO_FRAME_US", p.frameUs);
        p.minPixels = envNumber("NMB_ASYNC_PSO_MIN_PIXELS", p.minPixels);
        p.threads =
            (unsigned)std::max<uint64_t>(1, std::min<uint64_t>(4, envNumber("NMB_ASYNC_PSO_THREADS", p.threads)));
        if (const char *o = getenv("NMB_ASYNC_PSO_ONSCREEN"))
            p.onscreen = !strcmp(o, "all") ? 1 : !strcmp(o, "none") ? 2 : 0;
        if (p.on || p.api)
            NSLog(@"native-metal: async pipelines: sync API %s (wait %llu us, frame %llu us, onscreen %s, redraw %s), "
                  @"completion API %s, %u workers",
                  p.on ? "on" : "off", (unsigned long long)p.waitUs, (unsigned long long)p.frameUs,
                  p.onscreen == 1   ? "all"
                  : p.onscreen == 2 ? "none"
                                    : "surfaces",
                  p.redraw ? "on" : "off", p.api ? "on" : "off", p.threads);
    });
    return p;
}

// The counters behind the rates line's apso= part, taken (and cleared) by appendRates.
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
    bool skipped = false;  // a draw was skipped for it: draw the displays again once it is made
    bool lazy = false;     // startLazy's, not asked for by a set yet
    uint64_t asked = 0;
    pthread_t runner = nullptr;  // the thread making it, once one does
    qos_class_t runnerClass = QOS_CLASS_UNSPECIFIED;
    __strong id result = nil;  // the state the synchronous path made
    std::function<id(NSString **failure)> work;
    std::string label;

    bool done() const
    {
        return state == Made || state == Failed;
    }
};

void scheduleRedraw();

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
    bool skipped;
    {
        std::lock_guard<std::mutex> g(m->lock);
        m->result = made;
        m->state = made ? Making::Made : Making::Failed;
        skipped = m->skipped;
    }
    m->finished.notify_all();
    Counters &c = counters();
    (made ? c.made : c.failed)++;
    const uint64_t took = usNow() - m->asked;
    c.readyN++;
    c.readyUs += took;
    raiseMax(c.readyMaxUs, took);
    if (made && skipped)
        scheduleRedraw();
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
        if (idle == 0 && threads < policy().threads) {
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
    m->asked = usNow();
    counters().started++;
    workers().submit([m] {
        run(m);
    });
}

// Hand out `m` without making it: its first set does (wanted()).
inline void startLazy(const std::shared_ptr<Making> &m)
{
    m->lazy = true;
    counters().started++;
    counters().lazy++;
}

// A set of `m` (under m->lock): a lazy creation is asked for from now, and a pass that skips hands it to the workers.
inline void wanted(const std::shared_ptr<Making> &m, bool skips)
{
    if (!m->lazy)
        return;
    m->lazy = false;
    m->asked = usNow();
    counters().lazySet++;
    if (skips)
        workers().submit([m] {
            run(m);
        });
}

// This thread's waits in the current frame (16.7 ms of its time from its first wait).
struct FrameWaits {
    uint64_t start = 0, waited = 0;
};

inline FrameWaits &frameWaits()
{
    static thread_local FrameWaits f;
    return f;
}

// While the calling thread waits for `m` (`lock` held, as `m->lock`'s guard), the thread making it runs at least at the
// caller's class: a render thread (no class: Core Animation's is a fixed priority) lends the user-interactive class.
struct Lend {
    pthread_override_t override = nullptr;

    Lend(const Making &m)
    {
        if (m.state != Making::Running || !m.runner || pthread_equal(m.runner, pthread_self()))
            return;
        qos_class_t mine = qos_class_self();
        if (mine == QOS_CLASS_UNSPECIFIED)
            mine = QOS_CLASS_USER_INTERACTIVE;
        const qos_class_t runner = m.runnerClass == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : m.runnerClass;
        if (mine > runner)
            override = pthread_override_qos_class_start_np(m.runner, mine, 0);
    }

    ~Lend()
    {
        if (override)
            pthread_override_qos_class_end_np(override);
    }
};

// The display's pass (skips) or another (waits): `pixels` of its first color attachment, `surface` when an IOSurface's.
inline bool onscreen(bool surface, uint64_t pixels)
{
    const Policy &p = policy();
    return p.onscreen == 1 || (p.onscreen == 0 && surface && pixels >= p.minPixels);
}

// A set of `m` in a pass that skips (`skips`) or waits: true when it is made (set it), false when the set gives up (skip the
// draws until the next set).
inline bool ready(const std::shared_ptr<Making> &m, bool skips)
{
    {
        std::lock_guard<std::mutex> g(m->lock);
        if (m->state == Making::Made)
            return true;
        if (m->state == Making::Failed) {
            counters().skippedSets++;
            return false;
        }
        wanted(m, skips);
    }
    Counters &c = counters();
    c.pending++;
    const uint64_t began = usNow();
    if (!skips) {
        // Waited out: run it here when no worker took it yet, else wait for the worker.
        run(m);
        std::unique_lock<std::mutex> g(m->lock);
        Lend lend(*m);
        // Bounded, as is every userland wait: a making past a minute is a hang, not a compile.
        if (!m->finished.wait_for(g, std::chrono::seconds(60), [&] {
                return m->done();
            })) {
            os_log_fault(OS_LOG_DEFAULT,
                         "native-metal: render pipeline state %{public}s not made after 60 s: abort() for the stack",
                         m->label.c_str());
            abort();
        }
        const uint64_t took = usNow() - began;
        c.offWaits++;
        c.offWaitUs += took;
        if (m->state == Making::Failed)
            c.skippedSets++;
        return m->state == Making::Made;
    }
    const Policy &p = policy();
    FrameWaits &f = frameWaits();
    if (began - f.start > 16667) {
        f.start = began;
        f.waited = 0;
    }
    const uint64_t allowed = std::min(p.waitUs, p.frameUs > f.waited ? p.frameUs - f.waited : 0);
    std::unique_lock<std::mutex> g(m->lock);
    if (allowed && !m->done()) {
        Lend lend(*m);
        m->finished.wait_for(g, std::chrono::microseconds(allowed), [&] {
            return m->done();
        });
    }
    const uint64_t took = usNow() - began;
    f.waited += took;
    c.waitedUs += took;
    raiseMax(c.waitedMaxUs, took);
    if (m->state == Making::Made) {
        c.caught++;
        return true;
    }
    m->skipped = true;
    c.skippedSets++;
    return false;
}

// Draw the render server's displays again, whole (the header's notes): on the main queue, at most once per 16 ms.
inline void redrawDisplays()
{
    Class server = objc_getClass("CAWindowServer");
    SEL running = sel_registerName("serverIfRunning"), displays = sel_registerName("displays");
    SEL get = sel_registerName("invertsColors"), set = sel_registerName("setInvertsColors:");
    if (!server || ![server respondsToSelector:running])
        return;
    id s = ((id (*)(id, SEL))objc_msgSend)(server, running);
    if (!s || ![s respondsToSelector:displays])
        return;
    NSArray *list = ((id (*)(id, SEL))objc_msgSend)(s, displays);
    for (id d in list) {
        if (![d respondsToSelector:get] || ![d respondsToSelector:set])
            continue;
        const BOOL inverted = ((BOOL (*)(id, SEL))objc_msgSend)(d, get);
        ((void (*)(id, SEL, BOOL))objc_msgSend)(d, set, inverted);
    }
    counters().redraws++;
}

inline void scheduleRedraw()
{
    if (!policy().redraw)
        return;
    static std::atomic<bool> scheduled{false};
    if (scheduled.exchange(true))
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 16 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        scheduled = false;
        @autoreleasepool {
            redrawDisplays();
        }
    });
}

// The rates line's part (taken: each window's own): apso=<handed>/<made>/<refused> set=<found being made>/<made in the
// bounded wait> wait=<total>/<max>ms skip=<sets given up>/<draws skipped> off=<waited out>/<ms> rdy=<mean>/<max>ms
// redraw=<n> lazy=<lazy creations handed out>/<lazy creations a set asked for>. Nothing while no creation was handed and
// no set found one being made.
inline size_t appendRates(char *out, size_t size)
{
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
}

}  // namespace nbasync
