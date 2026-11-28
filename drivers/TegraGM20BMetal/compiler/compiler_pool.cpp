// SPDX-License-Identifier: MIT
// See compiler_pool.h.
#include "compiler_pool.h"

#include <pthread/qos.h>
#include <algorithm>

namespace switch_compiler {

Band bandOf(uint32_t qos, bool waited)
{
    if (!waited)
        return Band::Background;
    switch (qos) {
    case QOS_CLASS_UTILITY:
        return Band::Utility;
    case QOS_CLASS_DEFAULT:
        return Band::Default;
    case QOS_CLASS_USER_INITIATED:
        return Band::Initiated;
    case QOS_CLASS_BACKGROUND:
        return Band::Background;
    default:
        return Band::Interactive;
    }
}

uint32_t qosOf(Band band)
{
    static const qos_class_t classes[] = {QOS_CLASS_BACKGROUND, QOS_CLASS_UTILITY, QOS_CLASS_DEFAULT,
                                          QOS_CLASS_USER_INITIATED, QOS_CLASS_USER_INTERACTIVE};
    return classes[unsigned(band)];
}

Pool::Pool(Hooks hooks, unsigned workers) : hooks_(std::move(hooks))
{
    if (hooks_.idle)
        idleWorker_ = std::thread([this] {
            idleLoop();
        });
    for (unsigned i = 0; i < workers; ++i)
        workers_.emplace_back([this, slot = i + 1] {
            workerLoop(slot);
        });
}

Pool::~Pool()
{
    {
        std::lock_guard<std::mutex> l(lock_);
        stopping_ = true;
    }
    work_.notify_all();
    idleWork_.notify_all();
    for (std::thread &t : workers_)
        t.join();
    if (idleWorker_.joinable())
        idleWorker_.join();
}

namespace {

bool isReplayWaiter(const Waiter &w)
{
    return w.replay || (!w.id && !w.client);
}

}  // namespace

bool Pool::submit(const Name &name, std::unique_ptr<Work> work, const Waiter &asked)
{
    Waiter waiter = asked;
    const bool replay = isReplayWaiter(waiter);
    std::unique_lock<std::mutex> l(lock_);
    Name key = name;
    auto found = jobs_.find(key);
    for (uint64_t k = 1; found != jobs_.end() && hooks_.same && !hooks_.same(*found->second->work, *work); ++k) {
        key = name;
        for (size_t i = 0; i < sizeof k; ++i)
            key[i] ^= uint8_t((k * 0x9e3779b97f4a7c15ull) >> (8 * i));
        found = jobs_.find(key);
    }
    if (found == jobs_.end()) {
        auto job = std::make_shared<Job>();
        job->name = key;
        job->work = std::move(work);
        job->waiters.push_back(waiter);
        job->replay = replay;
        job->band = (replay || waiter.split) ? Band::Background : waiter.band;
        job->order = ++arrivals_;
        job->client = waiter.client;
        jobs_.emplace(key, job);
        stats_.jobs++;
        l.unlock();
        work_.notify_one();
        return true;
    }
    Job &job = *found->second;
    stats_.joined++;
    waiter.joined = true;
    if (job.primaryDone && !waiter.split) {
        const int32_t result = job.primaryResult;
        l.unlock();
        hooks_.complete(waiter, result);
        return false;
    }
    const bool promotedFromReplay = job.replay && !replay;
    if (promotedFromReplay)
        job.replay = false;
    job.waiters.push_back(waiter);
    const uint32_t serveSlot = job.running && !job.primaryDone && waiter.id ? job.workerSlot : 0;
    const Band band = (replay || waiter.split) ? Band::Background : waiter.band;
    const Band prevBand = job.band;
    const bool raisedBand = band > job.band;
    if (raisedBand)
        job.band = band;
    const bool wake = !job.running && (promotedFromReplay || raisedBand);
    if (job.running && !job.primaryDone) {
        if (raisedBand) {
            if (void *token = hooks_.raise(job.worker, band))
                job.raises.push_back(token);
            stats_.raised++;
        } else if (promotedFromReplay && prevBand == Band::Background) {
            if (void *token = hooks_.raise(job.worker, Band::Utility))
                job.raises.push_back(token);
            stats_.raised++;
        }
    }
    l.unlock();
    if (wake)
        work_.notify_one();
    if (serveSlot && hooks_.serve)
        hooks_.serve(waiter.id, serveSlot);
    return false;
}

// lock held: the most urgent queued job a free worker may start now
std::shared_ptr<Pool::Job> Pool::next()
{
    const auto busy = [&](uint64_t client) {
        for (const auto &[name, job] : jobs_)
            if (job->running && job->client == client)
                return true;
        return false;
    };
    bool liveActive = foregroundRunning_ > 0 || backgroundStarting_ > 0;
    if (!liveActive) {
        for (const auto &[name, job] : jobs_)
            if (!job->running && !job->replay) {
                liveActive = true;
                break;
            }
    }
    std::shared_ptr<Job> best;
    bool bestBusy = false;
    for (const auto &[name, job] : jobs_) {
        if (job->running)
            continue;
        if (job->replay) {
            if (liveActive || replayStarting_ >= kReplayStarts)
                continue;
        } else if (job->band > Band::Background) {
            if (foregroundRunning_ >= kForeground)
                continue;
        } else {
            if (backgroundStarting_ >= kBackgroundStarts)
                continue;
        }
        const bool clientBusy = !job->replay && busy(job->client);
        if (!best || (best->replay && !job->replay) ||
            (job->replay == best->replay &&
             (job->band > best->band ||
              (job->band == best->band &&
               (bestBusy > clientBusy || (bestBusy == clientBusy && job->order < best->order)))))) {
            best = job;
            bestBusy = clientBusy;
        }
    }
    return best;
}

void Pool::releaseLane(Job &job)
{
    switch (job.lane) {
    case Job::Lane::Foreground:
        foregroundRunning_--;
        break;
    case Job::Lane::Background:
        backgroundStarting_--;
        break;
    case Job::Lane::Replay:
        replayStarting_--;
        break;
    case Job::Lane::None:
        break;
    }
    job.lane = Job::Lane::None;
}

void Pool::workerLoop(unsigned slot)
{
    if (hooks_.registerWorker)
        hooks_.registerWorker(slot);
    hooks_.setBand(Band::Interactive);
    std::unique_lock<std::mutex> l(lock_);
    for (;;) {
        std::shared_ptr<Job> job;
        work_.wait(l, [&] {
            return stopping_ || (job = next());
        });
        if (!job)
            return;
        job->running = true;
        job->worker = pthread_self();
        job->workerSlot = slot;
        if (job->replay) {
            job->lane = Job::Lane::Replay;
            replayStarting_++;
        } else if (job->band > Band::Background) {
            job->lane = Job::Lane::Foreground;
            foregroundRunning_++;
        } else {
            job->lane = Job::Lane::Background;
            backgroundStarting_++;
        }
        const Band band = job->band;
        std::vector<uint64_t> ids;
        if (hooks_.serve) {
            ids.reserve(job->waiters.size());
            for (const Waiter &w : job->waiters)
                if (w.id)
                    ids.push_back(w.id);
        }
        l.unlock();
        for (uint64_t id : ids)
            hooks_.serve(id, slot);
        hooks_.setBand(band);
        const int32_t split = hooks_.run(*job->work, [&](int32_t result) {
            primary(job, result);
        });
        finish(job, split);
        hooks_.setBand(Band::Interactive);
        l.lock();
    }
}

void Pool::primary(const std::shared_ptr<Job> &job, int32_t result)
{
    std::vector<Waiter> answered;
    std::vector<void *> raises;
    bool splitWaited = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        job->primaryDone = true;
        job->primaryResult = result;
        auto split = std::stable_partition(job->waiters.begin(), job->waiters.end(), [](const Waiter &w) {
            return w.split;
        });
        answered.assign(split, job->waiters.end());
        job->waiters.erase(split, job->waiters.end());
        splitWaited = !job->waiters.empty();
        raises.swap(job->raises);
        releaseLane(*job);
        job->band = Band::Background;
    }
    work_.notify_all();
    for (const Waiter &w : answered)
        hooks_.complete(w, result);
    for (void *token : raises)
        hooks_.lower(token);
    if (splitWaited)
        hooks_.setBand(Band::Background);
}

void Pool::finish(const std::shared_ptr<Job> &job, int32_t splitResult)
{
    std::vector<Waiter> answered;
    std::vector<void *> raises;
    int32_t primaryResult = splitResult;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (!job->primaryDone)
            releaseLane(*job);
        primaryResult = job->primaryDone ? job->primaryResult : splitResult;
        answered.swap(job->waiters);
        raises.swap(job->raises);
        jobs_.erase(job->name);
        finishing_++;
    }
    work_.notify_all();
    for (const Waiter &w : answered)
        hooks_.complete(w, w.split ? splitResult : primaryResult);
    for (void *token : raises)
        hooks_.lower(token);
    bool idle = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        finishing_--;
        idle = jobs_.empty() && finishing_ == 0;
        if (idle && hooks_.idle)
            ++idleEpoch_;
    }
    idle_.notify_all();
    if (idle && hooks_.idle)
        idleWork_.notify_one();
}

void Pool::idleLoop()
{
    hooks_.setBand(Band::Background);
    std::unique_lock<std::mutex> l(lock_);
    for (;;) {
        idleWork_.wait(l, [&] {
            return stopping_ || (idleEpoch_ != idleHandled_ && jobs_.empty() && finishing_ == 0);
        });
        if (stopping_)
            return;
        idleHandled_ = idleEpoch_;
        l.unlock();
        hooks_.setBand(Band::Background);
        hooks_.idle();
        l.lock();
    }
}

void Pool::drain()
{
    std::unique_lock<std::mutex> l(lock_);
    idle_.wait(l, [&] {
        return jobs_.empty() && finishing_ == 0;
    });
}

Pool::Stats Pool::stats() const
{
    std::lock_guard<std::mutex> l(lock_);
    return stats_;
}

}  // namespace switch_compiler
