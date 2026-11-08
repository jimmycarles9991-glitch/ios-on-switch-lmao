// SPDX-License-Identifier: MIT
// See raw_compiler.h.
#include "raw_compiler.h"

#include <dispatch/dispatch.h>
#include <time.h>

namespace raw {
namespace {

uint64_t nowUs()
{
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
}

}  // namespace

struct Compiler::Job {
    Compiler *compiler;
    CacheKey key;
};

Compiler::Compiler(RecordCache *cache, Make make, uint64_t retryUs)
    : cache_(cache), make_(std::move(make)), retryUs_(retryUs),
      queue_(dispatch_queue_create(
          "com.brunostjohn.switch.raw.compile",
          dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)))
{}

Compiler::~Compiler()
{
    drain();
    dispatch_release(static_cast<dispatch_queue_t>(queue_));
}

Compiler::State Compiler::ask(const CacheKey &k, Done done, const RuntimeRecord **record)
{
    std::lock_guard<std::mutex> l(lock_);
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
    const uint64_t began = nowUs();
    {
        std::lock_guard<std::mutex> l(lock_);
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
    const RecordCache::Found found = cache_ ? cache_->load(k, *loaded, why) : RecordCache::Found::None;
    if (found == RecordCache::Found::None)
        return found;
    if (found == RecordCache::Found::Record)
        record = *loaded;
    std::lock_guard<std::mutex> l(lock_);
    stats_.hits++;
    stats_.lookupHits++;
    stats_.lookupUs += nowUs() - began;
    // an ask that came in since keeps its own entry (its job finds the same entry on disk)
    if (const auto [it, first] = entries_.try_emplace(k.name()); first) {
        it->second.state = found == RecordCache::Found::Record ? State::Ready : State::Refused;
        it->second.why = why;
        it->second.record = found == RecordCache::Found::Record ? std::move(loaded) : nullptr;
    }
    return found;
}

bool Compiler::retryAfter(const CacheKey &k, uint64_t &us) const
{
    std::lock_guard<std::mutex> l(lock_);
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
    std::lock_guard<std::mutex> m(making_);
    found = cache_ ? cache_->load(k, record, why) : RecordCache::Found::None;
    const Outcome outcome = found == RecordCache::Found::Record    ? Outcome::Made
                            : found == RecordCache::Found::Refusal ? Outcome::Refused
                                                                   : make_(k, record, why);
    if (found == RecordCache::Found::None && outcome != Outcome::Failed && cache_)
        cache_->store(k, outcome == Outcome::Made ? &record : nullptr, why);
    return outcome;
}

// The state's entry set to `outcome`; the asks that waited for it, and in *made its record (null: none)
std::vector<Compiler::Done> Compiler::settle(const CacheKey &k, Outcome outcome, RecordCache::Found found,
                                             std::unique_ptr<RuntimeRecord> record, const std::string &why,
                                             uint64_t began, const RuntimeRecord **made)
{
    std::lock_guard<std::mutex> l(lock_);
    Entry &e = entries_[k.name()];
    std::vector<Done> waiters;
    waiters.swap(e.waiters);
    e.why = why;
    *made = nullptr;
    if (outcome == Outcome::Made) {
        e.record = std::move(record);
        *made = e.record.get();
    }
    (found == RecordCache::Found::None ? stats_.misses : stats_.hits)++;
    stats_.refusals += outcome == Outcome::Refused;
    stats_.failures += outcome == Outcome::Failed;
    stats_.makeUs += nowUs() - began;
    if (outcome == Outcome::Failed) {
        e.failures++;
        e.failedUs = nowUs();
    }
    e.state = outcome == Outcome::Made ? State::Ready : outcome == Outcome::Refused ? State::Refused : State::Failed;
    return waiters;
}

void Compiler::run(void *context)
{
    std::unique_ptr<Job> job(static_cast<Job *>(context));
    Compiler &c = *job->compiler;
    const uint64_t began = nowUs();
    auto record = std::make_unique<RuntimeRecord>();
    std::string why;
    RecordCache::Found found;
    const Outcome outcome = c.produce(job->key, *record, why, found);
    const RuntimeRecord *made = nullptr;
    for (const Done &done : c.settle(job->key, outcome, found, std::move(record), why, began, &made))
        done(made, why);
    std::lock_guard<std::mutex> l(c.lock_);
    c.queued_--;
    c.idle_.notify_all();
}

Compiler::State Compiler::makeNow(const CacheKey &k, RuntimeRecord &record, std::string &why)
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
        std::lock_guard<std::mutex> l(lock_);
        const auto [it, first] = entries_.try_emplace(k.name());
        Entry &e = it->second;
        uint64_t wait = 0;
        if (!first && (e.state != State::Failed || !retryable(e, wait) || wait)) {
            if (e.state == State::Ready)
                record = *e.record;
            return e.state;
        }
        e.state = State::Pending;
    }
    const uint64_t began = nowUs();
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

void Compiler::drain()
{
    std::unique_lock<std::mutex> l(lock_);
    idle_.wait(l, [&] {
        return queued_ == 0;
    });
}

Compiler::Stats Compiler::stats() const
{
    std::lock_guard<std::mutex> l(lock_);
    return stats_;
}

}  // namespace raw
