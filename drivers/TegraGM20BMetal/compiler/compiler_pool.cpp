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
    for (unsigned i = 0; i < workers; ++i)
        workers_.emplace_back([this] {
            workerLoop();
        });
}

Pool::~Pool()
{
    {
        std::lock_guard<std::mutex> l(lock_);
        stopping_ = true;
    }
    work_.notify_all();
    for (std::thread &t : workers_)
        t.join();
}

bool Pool::submit(const Name &name, std::unique_ptr<Work> work, const Waiter &asked)
{
    Waiter waiter = asked;
    std::unique_lock<std::mutex> l(lock_);
    const auto found = jobs_.find(name);
    if (found == jobs_.end()) {
        auto job = std::make_shared<Job>();
        job->name = name;
        job->work = std::move(work);
        job->waiters.push_back(waiter);
        job->band = waiter.split ? Band::Background : waiter.band;
        job->order = ++arrivals_;
        job->client = waiter.client;
        jobs_.emplace(name, job);
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
    job.waiters.push_back(waiter);
    const Band band = waiter.split ? Band::Background : waiter.band;
    if (band <= job.band)
        return false;
    job.band = band;
    if (job.running && !job.primaryDone) {
        if (void *token = hooks_.raise(job.worker, band))
            job.raises.push_back(token);
        stats_.raised++;
    }
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
    std::shared_ptr<Job> best;
    bool bestBusy = false;
    for (const auto &[name, job] : jobs_) {
        if (job->running)
            continue;
        const bool foreground = job->band > Band::Background;
        if (foreground ? foregroundRunning_ >= kForeground : backgroundStarting_ >= kBackgroundStarts)
            continue;
        const bool clientBusy = busy(job->client);
        if (!best || job->band > best->band ||
            (job->band == best->band &&
             (bestBusy > clientBusy || (bestBusy == clientBusy && job->order < best->order)))) {
            best = job;
            bestBusy = clientBusy;
        }
    }
    return best;
}

void Pool::workerLoop()
{
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
        job->foreground = job->band > Band::Background;
        (job->foreground ? foregroundRunning_ : backgroundStarting_)++;
        const Band band = job->band;
        l.unlock();
        hooks_.setBand(band);
        const int32_t split = hooks_.run(*job->work, [&](int32_t result) {
            primary(job, result);
        });
        finish(job, split);
        l.lock();
    }
}

void Pool::primary(const std::shared_ptr<Job> &job, int32_t result)
{
    std::vector<Waiter> answered;
    std::vector<void *> raises;
    {
        std::lock_guard<std::mutex> l(lock_);
        job->primaryDone = true;
        job->primaryResult = result;
        auto split = std::stable_partition(job->waiters.begin(), job->waiters.end(), [](const Waiter &w) {
            return w.split;
        });
        answered.assign(split, job->waiters.end());
        job->waiters.erase(split, job->waiters.end());
        raises.swap(job->raises);
        if (job->foreground) {
            foregroundRunning_--;
            job->foreground = false;
        } else {
            backgroundStarting_--;
        }
        job->band = Band::Background;
    }
    work_.notify_all();
    hooks_.setBand(Band::Background);
    for (void *token : raises)
        hooks_.lower(token);
    for (const Waiter &w : answered)
        hooks_.complete(w, result);
}

void Pool::finish(const std::shared_ptr<Job> &job, int32_t splitResult)
{
    std::vector<Waiter> answered;
    std::vector<void *> raises;
    int32_t primaryResult = splitResult;
    bool idle = false;
    {
        std::lock_guard<std::mutex> l(lock_);
        if (!job->primaryDone) {
            if (job->foreground)
                foregroundRunning_--;
            else
                backgroundStarting_--;
        }
        primaryResult = job->primaryDone ? job->primaryResult : splitResult;
        answered.swap(job->waiters);
        raises.swap(job->raises);
        jobs_.erase(job->name);
        idle = jobs_.empty();
    }
    work_.notify_all();
    idle_.notify_all();
    for (void *token : raises)
        hooks_.lower(token);
    for (const Waiter &w : answered)
        hooks_.complete(w, w.split ? splitResult : primaryResult);
    if (idle && hooks_.idle)
        hooks_.idle();
}

void Pool::drain()
{
    std::unique_lock<std::mutex> l(lock_);
    idle_.wait(l, [&] {
        return jobs_.empty();
    });
}

Pool::Stats Pool::stats() const
{
    std::lock_guard<std::mutex> l(lock_);
    return stats_;
}

}  // namespace switch_compiler
