// SPDX-License-Identifier: MIT
// The system shader compiler's one compile pool, sized for the Switch's four A57s beside the compositor: four workers, at
// most two compiles above the background class at once and one background compile started at once (a split form finishing
// in the background takes a worker, not the background start). A job is one record: every request for it, from any
// process, waits on that one compile. It runs at the class of its most urgent waiter, and a waiter more urgent than a
// running job raises its worker until the job's primary record is published; then the job's own waiters are answered and
// the worker drops to the background class for the split form. A request nobody waits for runs at the background class.
// Among jobs of one class, a client with none running goes first, then the oldest: one process asking for many states
// does not hold the others back by more than the compile it already has running.
// Workers park on a condition until there is a job; nothing polls. They park at the interactive class, so the one woken
// for an urgent job runs at once on a busy system, and take the job's class once they have it.
#pragma once
#include <pthread.h>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace switch_compiler {

using Name = std::array<uint8_t, 32>;

enum class Band : uint8_t {
    Background,
    Utility,
    Default,
    Initiated,
    Interactive
};

// The class a request runs at: its waiter's; a fixed-priority waiter (no class: Core Animation's render thread) is the
// most urgent there is; a request nobody waits for is background work
Band bandOf(uint32_t qos, bool waited);
uint32_t qosOf(Band band);

struct Waiter {
    uint64_t id = 0, client = 0;
    Band band = Band::Background;
    bool split = false;   // waits for the split form, not just the record
    bool joined = false;  // came while another request's compile of its record was queued or running
    bool replay = false;  // boot-replay prewarm with no live process waiting
};

// What a job compiles; the pool only hands it to Hooks::run
struct Work {
    virtual ~Work() = default;
};

class Pool {
public:
    struct Hooks {
        // The job's compile, on a worker: `primary(result)` once its record (or refusal) is published, then the split
        // form; returns the split's result
        std::function<int32_t(Work &work, const std::function<void(int32_t)> &primary)> run;
        std::function<void(const Waiter &waiter, int32_t result)> complete;
        // The calling worker's own class
        std::function<void(Band band)> setBand;
        // A running job's worker raised to `band` until lower(token)
        std::function<void *(pthread_t worker, Band band)> raise;
        std::function<void(void *token)> lower;
        // Called once on each worker thread with its 1-based slot (1..kWorkers) before taking jobs
        std::function<void(uint32_t worker)> registerWorker;
        // Whether two works compile one record: a request joins a job of its name only then. Names are what any client
        // can steer (store_key.h), so a job of another content under the same name runs apart, under the next free key.
        // None: every job of a name is the same.
        std::function<bool(const Work &a, const Work &b)> same;
        // Bind kernel request `id` to the worker registered in `worker` (1..kWorkers)
        std::function<void(uint64_t id, uint32_t worker)> serve;
        // On the worker whose job was the last one queued or running, after its waiters were answered, at the background
        // class
        std::function<void()> idle;
    };

    static constexpr unsigned kWorkers = 4, kForeground = 2, kBackgroundStarts = 1, kReplayStarts = 1;

    explicit Pool(Hooks hooks, unsigned workers = kWorkers);
    ~Pool();

    // `waiter` waits for the record `name`: on its job if one is queued or running (`work` is dropped), else on a new job of
    // `work`. False: it joined a job.
    bool submit(const Name &name, std::unique_ptr<Work> work, const Waiter &waiter);
    // Every job submitted so far has ended (host tests)
    void drain();

    struct Stats {
        uint64_t jobs = 0, joined = 0, raised = 0;
    };

    Stats stats() const;

private:
    struct Job {
        enum class Lane : uint8_t {
            None,
            Replay,
            Background,
            Foreground
        };

        Name name;
        std::unique_ptr<Work> work;
        std::vector<Waiter> waiters;
        Band band = Band::Background;
        uint64_t order = 0, client = 0;  // its arrival, and the client that asked first
        bool running = false, primaryDone = false, replay = false;
        Lane lane = Lane::None;
        int32_t primaryResult = 0;
        pthread_t worker = nullptr;
        uint32_t workerSlot = 0;
        std::vector<void *> raises;
    };

    Hooks hooks_;
    mutable std::mutex lock_;
    std::condition_variable work_, idle_, idleWork_;
    std::map<Name, std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> workers_;
    std::thread idleWorker_;
    unsigned foregroundRunning_ = 0, backgroundStarting_ = 0, replayStarting_ = 0, finishing_ = 0;
    uint64_t arrivals_ = 0, idleEpoch_ = 0, idleHandled_ = 0;
    bool stopping_ = false;
    Stats stats_;

    void workerLoop(unsigned slot);
    void idleLoop();
    std::shared_ptr<Job> next();
    void releaseLane(Job &job);
    void primary(const std::shared_ptr<Job> &job, int32_t result);
    void finish(const std::shared_ptr<Job> &job, int32_t splitResult);
};

}  // namespace switch_compiler
