// SPDX-License-Identifier: MIT
// See raw_compiler.h.
#include "raw_lock.h"
#include "raw_compiler.h"

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <time.h>

namespace raw {
namespace {

uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

#if SWITCH_RAW_DIAGNOSTICS
thread_local uint64_t tSlotWaitUs;
#endif

}  // namespace

// A thread waiting for a slot lends its class to every holder below it for as long as it waits (a dispatch semaphore lends
// nothing). A thread of a fixed priority and no class (Core Animation's render thread) does not wait: a class lent to a
// daemon's workers stops at user-initiated, below it, so it compiles at once beyond the bound. A thread that holds a slot
// takes none again.
class CompileSlots {
public:
    explicit CompileSlots(unsigned count) : holders_(count)
    {}

    class Held {
    public:
        Held(CompileSlots *slots, size_t index) : slots_(slots), index_(index)
        {}

        Held(const Held &) = delete;
        Held &operator=(const Held &) = delete;

        ~Held()
        {
            if (slots_)
                slots_->give(index_);
        }

    private:
        CompileSlots *slots_;
        size_t index_;
    };

    Held take()
    {
#if SWITCH_RAW_DIAGNOSTICS
        const uint64_t began = nowUs();
        tSlotWaitUs = 0;
#endif
        if (tHeld == this)
            return Held(nullptr, 0);
        const qos_class_t mine = qos_class_self();
        std::unique_lock<raw::Mutex> l(lock_);
        for (;;) {
            for (size_t i = 0; i < holders_.size(); ++i)
                if (!holders_[i].thread) {
                    holders_[i] = {pthread_self(), qos_class_self()};
                    tHeld = this;
                    RAW_DIAGNOSTIC(tSlotWaitUs = nowUs() - began);
                    return Held(this, i);
                }
            if (mine == QOS_CLASS_UNSPECIFIED)
                return Held(nullptr, 0);
            std::vector<pthread_override_t> lent;
            for (const Holder &h : holders_)
                if (mine > (h.qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : h.qos))
                    if (pthread_override_t o = pthread_override_qos_class_start_np(h.thread, mine, 0))
                        lent.push_back(o);
            freed_.wait(l);
            for (pthread_override_t o : lent)
                pthread_override_qos_class_end_np(o);
        }
    }

private:
    struct Holder {
        pthread_t thread = nullptr;
        qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    };

    static thread_local const CompileSlots *tHeld;
    raw::Mutex lock_;
    raw::CondVar freed_;
    std::vector<Holder> holders_;

    void give(size_t index)
    {
        {
            std::lock_guard<raw::Mutex> l(lock_);
            holders_[index] = {};
            tHeld = nullptr;
        }
        freed_.notify_all();
    }
};

thread_local const CompileSlots *CompileSlots::tHeld = nullptr;

struct Compiler::Job {
    Compiler *compiler;
    CacheKey key;
};

struct Compiler::Store {
    RecordCache *cache;
    CacheKey key;
    std::unique_ptr<RuntimeRecord> record;
    std::string why;
};

struct Compiler::Task {
    Compiler *compiler;
    std::function<void()> work;
};

void Compiler::enqueue(std::function<void()> work)
{
    std::lock_guard<raw::Mutex> lock(lock_);
    ++queued_;
    dispatch_async_f(static_cast<dispatch_queue_t>(queue_), new Task{this, std::move(work)}, runTask);
}

void Compiler::runTask(void *context)
{
    std::unique_ptr<Task> task(static_cast<Task *>(context));
    Compiler &compiler = *task->compiler;

    struct Retire {
        Compiler &compiler;

        ~Retire()
        {
            std::lock_guard<raw::Mutex> lock(compiler.lock_);
            --compiler.queued_;
            compiler.idle_.notify_all();
        }
    } retire{compiler};

    const CompileSlots::Held slot = compiler.slots_->take();
    task->work();
}

Compiler::Compiler(RecordCache *cache, Make make, uint64_t retryUs)
    : cache_(cache), make_(std::move(make)), retryUs_(retryUs),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.compile",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_CONCURRENT, QOS_CLASS_USER_INITIATED, 0))),
      persistence_(
          dispatch_queue_create("com.brunostjohn.switch.raw.cache",
                                dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0))),
      slots_(std::make_unique<CompileSlots>(2))
{}

Compiler::~Compiler()
{
    drain();
    dispatch_release(static_cast<dispatch_queue_t>(queue_));
    dispatch_release(static_cast<dispatch_queue_t>(persistence_));
}

Compiler::State Compiler::ask(const CacheKey &k, Done done, const RuntimeRecord **record)
{
    std::lock_guard<raw::Mutex> l(lock_);
    const auto [it, first] = entries_.try_emplace(k.name());
    Entry &e = it->second;
    uint64_t wait = 0;
    if (first || (retryable(e, wait) && wait == 0)) {
        e.state = State::Pending;
        if (done)
            e.waiters.push_back(std::move(done));
        queue(k);
        return State::Pending;
    }
    if (e.state == State::Pending && done)
        e.waiters.push_back(std::move(done));
    if (record)
        *record = e.record.get();
    return e.state;
}

// lock_ held: whether `e` failed and may be tried again, and in `wait` how long until then
bool Compiler::retryable(const Entry &e, uint64_t &wait) const
{
    if (e.state != State::Failed || e.failures > kRetries)
        return false;
    const uint64_t due = e.failedUs + (retryUs_ << (2 * (e.failures - 1))), now = nowUs();
    wait = due > now ? due - now : 0;
    return true;
}

RecordCache::Found Compiler::lookup(const CacheKey &k, RuntimeRecord &record, std::string &why)
{
#if SWITCH_RAW_DIAGNOSTICS
    const uint64_t began = nowUs();
#endif
    {
        std::lock_guard<raw::Mutex> l(lock_);
        const auto it = entries_.find(k.name());
        if (it != entries_.end()) {
            const Entry &e = it->second;
            if (e.state == State::Ready)
                record = *e.record;
            why = e.why;
            return e.state == State::Ready     ? RecordCache::Found::Record
                   : e.state == State::Refused ? RecordCache::Found::Refusal
                                               : RecordCache::Found::None;
        }
    }
    auto loaded = std::make_unique<RuntimeRecord>();
    bool seeded = false;
    const RecordCache::Found found =
        cache_ ? cache_->load(k, *loaded, why, SWITCH_RAW_DIAGNOSTICS ? &seeded : nullptr) : RecordCache::Found::None;
    if (found == RecordCache::Found::None)
        return found;
    if (found == RecordCache::Found::Record)
        record = *loaded;
    std::lock_guard<raw::Mutex> l(lock_);
    RAW_DIAGNOSTIC(stats_.hits++; stats_.lookupHits++; (seeded ? stats_.seedHits : stats_.ownHits)++;
                   stats_.lookupUs += nowUs() - began);
    // an ask that came in since keeps its own entry (its job finds the same entry on disk)
    if (const auto [it, first] = entries_.try_emplace(k.name()); first) {
        it->second.state = found == RecordCache::Found::Record ? State::Ready : State::Refused;
        it->second.why = why;
        it->second.record = found == RecordCache::Found::Record ? std::move(loaded) : nullptr;
    }
    return found;
}

void Compiler::keep(const CacheKey &k, RecordCache::Found found, const RuntimeRecord &record, const std::string &why)
{
    if (found == RecordCache::Found::None)
        return;
    std::lock_guard<raw::Mutex> l(lock_);
    const auto [it, first] = entries_.try_emplace(k.name());
    if (!first)
        return;
    it->second.state = found == RecordCache::Found::Record ? State::Ready : State::Refused;
    it->second.why = why;
    if (found == RecordCache::Found::Record)
        it->second.record = std::make_unique<RuntimeRecord>(record);
}

bool Compiler::retryAfter(const CacheKey &k, uint64_t &us) const
{
    std::lock_guard<raw::Mutex> l(lock_);
    const auto it = entries_.find(k.name());
    return it != entries_.end() && retryable(it->second, us);
}

// lock_ held
void Compiler::queue(const CacheKey &k)
{
    queued_++;
    dispatch_async_f(static_cast<dispatch_queue_t>(queue_), new Job{this, k}, run);
}

// The disk's entry for `k`, else make_'s result (kept on disk unless the make could not run)
Compiler::Outcome Compiler::produce(const CacheKey &k, RuntimeRecord &record, std::string &why,
                                    RecordCache::Found &found)
{
    found = cache_ ? cache_->load(k, record, why) : RecordCache::Found::None;
    if (found != RecordCache::Found::None)
        return found == RecordCache::Found::Record ? Outcome::Made : Outcome::Refused;

    const CompileSlots::Held slot = slots_->take();
    RAW_DIAGNOSTIC(std::lock_guard<raw::Mutex> l(lock_); stats_.waitUs += tSlotWaitUs);
    const Outcome outcome = make_(k, record, why);
    if (found == RecordCache::Found::None && outcome != Outcome::Failed && cache_)
        dispatch_async_f(
            static_cast<dispatch_queue_t>(persistence_),
            new Store{cache_, k, outcome == Outcome::Made ? std::make_unique<RuntimeRecord>(record) : nullptr, why},
            store);
    return outcome;
}

void Compiler::store(void *context)
{
    std::unique_ptr<Store> pending(static_cast<Store *>(context));
    pending->cache->store(pending->key, pending->record.get(), pending->why);
}

// The state's entry set to `outcome`; the asks that waited for it, and in *made its record (null: none)
std::vector<Compiler::Done> Compiler::settle(const CacheKey &k, Outcome outcome, RecordCache::Found found,
                                             std::unique_ptr<RuntimeRecord> record, const std::string &why,
                                             uint64_t began, const RuntimeRecord **made)
{
    std::lock_guard<raw::Mutex> l(lock_);
    Entry &e = entries_[k.name()];
    std::vector<Done> waiters;
    waiters.swap(e.waiters);
    e.why = why;
    *made = nullptr;
    if (outcome == Outcome::Made) {
        e.record = std::move(record);
        *made = e.record.get();
    }
#if SWITCH_RAW_DIAGNOSTICS
    (found == RecordCache::Found::None ? stats_.misses : stats_.hits)++;
    stats_.refusals += outcome == Outcome::Refused;
    stats_.failures += outcome == Outcome::Failed;
    stats_.makeUs += nowUs() - began;
#else
    (void)found;
    (void)began;
#endif
    if (outcome == Outcome::Failed) {
        e.failures++;
        e.failedUs = nowUs();
    }
    e.state = outcome == Outcome::Made ? State::Ready : outcome == Outcome::Refused ? State::Refused : State::Failed;
    e.maker = nullptr;
    settled_.notify_all();
    return waiters;
}

void Compiler::run(void *context)
{
    std::unique_ptr<Job> job(static_cast<Job *>(context));
    Compiler &c = *job->compiler;
    {
        std::lock_guard<raw::Mutex> l(c.lock_);
        Entry &e = c.entries_[job->key.name()];
        if (e.state != State::Pending || e.maker) {
            c.queued_--;
            c.idle_.notify_all();
            return;
        }
        e.maker = pthread_self();
    }
    uint64_t began = 0;
    RAW_DIAGNOSTIC(began = nowUs());
    auto record = std::make_unique<RuntimeRecord>();
    std::string why;
    RecordCache::Found found;
    const Outcome outcome = c.produce(job->key, *record, why, found);
    const RuntimeRecord *made = nullptr;
    for (const Done &done : c.settle(job->key, outcome, found, std::move(record), why, began, &made))
        done(made, why);
    std::lock_guard<raw::Mutex> l(c.lock_);
    c.queued_--;
    c.idle_.notify_all();
}

Compiler::State Compiler::makeNow(const CacheKey &k, RuntimeRecord &record, std::string &why, bool again)
{
    switch (lookup(k, record, why)) {
    case RecordCache::Found::Record:
        return State::Ready;
    case RecordCache::Found::Refusal:
        return State::Refused;
    case RecordCache::Found::None:
        break;
    }
    {
        std::unique_lock<raw::Mutex> l(lock_);
        const auto [it, first] = entries_.try_emplace(k.name());
        Entry &e = it->second;
        if (!first && e.state == State::Pending && e.maker)
            waitForMaker(l, e);
        uint64_t wait = 0;
        if (!first && e.state != State::Pending &&
            (e.state != State::Failed || !retryable(e, wait) || (wait && !again))) {
            if (e.state == State::Ready)
                record = *e.record;
            return e.state;
        }
        e.state = State::Pending;
        e.maker = pthread_self();
    }
    uint64_t began = 0;
    RAW_DIAGNOSTIC(began = nowUs());
    auto made = std::make_unique<RuntimeRecord>();
    RecordCache::Found found;
    const Outcome outcome = produce(k, *made, why, found);
    if (outcome == Outcome::Made)
        record = *made;
    const RuntimeRecord *kept = nullptr;
    for (const Done &done : settle(k, outcome, found, std::move(made), why, began, &kept))
        done(kept, why);
    return outcome == Outcome::Made ? State::Ready : outcome == Outcome::Refused ? State::Refused : State::Failed;
}

// lock_ held in `l`: until the thread making `e` settles it, that thread runs at least at the caller's class
void Compiler::waitForMaker(std::unique_lock<raw::Mutex> &l, Entry &e)
{
    const qos_class_t mine = qos_class_self();
    pthread_override_t lent =
        mine == QOS_CLASS_UNSPECIFIED ? nullptr : pthread_override_qos_class_start_np(e.maker, mine, 0);
    settled_.wait(l, [&] {
        return e.state != State::Pending || !e.maker;
    });
    if (lent)
        pthread_override_qos_class_end_np(lent);
}

void Compiler::drain()
{
    std::unique_lock<raw::Mutex> l(lock_);
    idle_.wait(l, [&] {
        return queued_ == 0;
    });
    l.unlock();
    dispatch_sync_f(static_cast<dispatch_queue_t>(persistence_), nullptr, [](void *) {});
}

#if SWITCH_RAW_DIAGNOSTICS
uint64_t Compiler::slotWaitUs()
{
    return tSlotWaitUs;
}
#endif

Compiler::Stats Compiler::stats() const
{
#if SWITCH_RAW_DIAGNOSTICS
    std::lock_guard<raw::Mutex> l(lock_);
    return stats_;
#else
    return {};
#endif
}

}  // namespace raw
