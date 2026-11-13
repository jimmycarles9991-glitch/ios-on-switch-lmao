// SPDX-License-Identifier: MIT
#include "../AsyncPipelines.h"
#include <gtest/gtest.h>

TEST(AsyncPipelines, LazyObservationStartsWorkAndPublishesBeforeCallbacks)
{
    auto state = std::make_shared<nbasync::Making>();
    std::mutex lock;
    std::condition_variable changed;
    bool entered = false, release = false, notified = false;
    std::atomic<unsigned> makes{0};
    state->work = [&](NSString **) -> id {
        makes++;
        std::unique_lock<std::mutex> guard(lock);
        entered = true;
        changed.notify_all();
        changed.wait(guard, [&] {
            return release;
        });
        return [NSObject new];
    };
    nbasync::startLazy(state);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   EXPECT_EQ(nbasync::observe(state), 1);  // callback must run outside the state lock
                                   std::lock_guard<std::mutex> guard(lock);
                                   notified = true;
                                   changed.notify_all();
                               }),
              0);
    EXPECT_EQ(nbasync::observe(state), 0);
    {
        std::unique_lock<std::mutex> guard(lock);
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered;
        }));
        release = true;
        changed.notify_all();
        EXPECT_TRUE(changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return notified;
        }));
    }
    EXPECT_EQ(makes.load(), 1u);
    EXPECT_EQ(nbasync::observe(state), 1);
    EXPECT_TRUE(nbasync::ready(state));
}

TEST(AsyncPipelines, FailedCreationIsTerminalAndNotifiesEveryObserver)
{
    auto state = std::make_shared<nbasync::Making>();
    state->work = [](NSString **failure) -> id {
        *failure = @"test refusal";
        return nil;
    };
    unsigned callbacks = 0;
    // Non-lazy queued state: observation is separate from its executor, so register both before running it.
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              0);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              0);
    EXPECT_TRUE(nbasync::run(state));
    EXPECT_FALSE(nbasync::run(state));
    EXPECT_EQ(callbacks, 2u);
    EXPECT_EQ(nbasync::observe(state,
                               [&] {
                                   ++callbacks;
                               }),
              -1);
    EXPECT_EQ(callbacks, 2u);
    EXPECT_FALSE(nbasync::ready(state));
}

TEST(AsyncPipelines, ObservingCompletionCannotLoseTheNotification)
{
    for (unsigned i = 0; i < 100; ++i) {
        auto state = std::make_shared<nbasync::Making>();
        state->work = [](NSString **) -> id {
            return [NSObject new];
        };
        std::atomic<unsigned> callbacks{0};
        std::thread worker([state] {
            @autoreleasepool {
                nbasync::run(state);
            }
        });
        const int observed = nbasync::observe(state, [&] {
            callbacks++;
        });
        worker.join();
        EXPECT_TRUE(observed == 0 || observed == 1);
        EXPECT_EQ(callbacks.load(), observed == 0 ? 1u : 0u);
        EXPECT_EQ(nbasync::observe(state), 1);
    }
}

namespace {

// A creation whose work waits, on a worker, until the test lets it go
struct Held {
    std::mutex lock;
    std::condition_variable changed;
    unsigned entered = 0;
    bool release = false;

    std::shared_ptr<nbasync::Making> make()
    {
        auto state = std::make_shared<nbasync::Making>();
        state->work = [this](NSString **) -> id {
            std::unique_lock<std::mutex> guard(lock);
            ++entered;
            changed.notify_all();
            changed.wait(guard, [&] {
                return release;
            });
            return [NSObject new];
        };
        return state;
    }

    bool waitEntered(unsigned n)
    {
        std::unique_lock<std::mutex> guard(lock);
        return changed.wait_for(guard, std::chrono::seconds(5), [&] {
            return entered >= n;
        });
    }

    void letGo()
    {
        std::lock_guard<std::mutex> guard(lock);
        release = true;
        changed.notify_all();
    }
};

bool made(const std::shared_ptr<nbasync::Making> &state)
{
    for (int i = 0; i < 500; ++i) {
        if (nbasync::observe(state) == 1)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

}  // namespace

TEST(AsyncPipelines, AnUrgedCreationIsMadeAheadOfTheCreationsQueuedBeforeIt)
{
    Held busy;
    const unsigned workers = nbasync::policy().threads;
    std::vector<std::shared_ptr<nbasync::Making>> before;
    for (unsigned i = 0; i < workers + 2; ++i) {
        before.push_back(busy.make());
        nbasync::start(before.back());
    }
    ASSERT_TRUE(busy.waitEntered(workers));
    auto wanted = std::make_shared<nbasync::Making>();
    std::atomic<unsigned> makes{0};
    wanted->work = [&](NSString **) -> id {
        makes++;
        return [NSObject new];
    };
    nbasync::start(wanted);
    EXPECT_EQ(nbasync::urge(wanted), 1);
    EXPECT_EQ(nbasync::observe(wanted), 1);
    {
        std::lock_guard<std::mutex> guard(busy.lock);
        EXPECT_EQ(busy.entered, workers);
    }
    busy.letGo();
    for (const auto &state : before)
        EXPECT_TRUE(made(state));
    EXPECT_EQ(makes.load(), 1u);
}

TEST(AsyncPipelines, UrgingACreationAWorkerIsMakingLendsItTheUrgersClassUntilItIsMade)
{
    ASSERT_EQ(pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0), 0);
    Held held;
    auto state = held.make();
    nbasync::start(state);
    ASSERT_TRUE(held.waitEntered(1));
    EXPECT_EQ(nbasync::urge(state), 0);
    {
        std::lock_guard<std::mutex> guard(state->lock);
        EXPECT_NE(state->urged, nullptr);
    }
    EXPECT_EQ(nbasync::urge(state), 0);
    held.letGo();
    EXPECT_TRUE(made(state));
    std::lock_guard<std::mutex> guard(state->lock);
    EXPECT_EQ(state->urged, nullptr);
}

TEST(AsyncPipelines, AnUrgedLazyCreationIsMadeAtOnceAndAFailureAnswersAtOnce)
{
    auto lazy = std::make_shared<nbasync::Making>();
    lazy->work = [](NSString **) -> id {
        return [NSObject new];
    };
    nbasync::startLazy(lazy);
    EXPECT_EQ(nbasync::urge(lazy), 1);
    auto refused = std::make_shared<nbasync::Making>();
    refused->work = [](NSString **failure) -> id {
        *failure = @"test refusal";
        return nil;
    };
    nbasync::startLazy(refused);
    EXPECT_EQ(nbasync::urge(refused), -1);
    EXPECT_EQ(nbasync::urge(refused), -1);
}

TEST(AsyncPipelines, AClaimedLazyCreationIsLeftToItsUrgerWhileWorkersIdle)
{
    auto state = std::make_shared<nbasync::Making>();
    std::atomic<pthread_t> ranOn{nullptr};
    state->work = [&](NSString **) -> id {
        ranOn = pthread_self();
        return [NSObject new];
    };
    nbasync::startLazy(state);
    std::atomic<unsigned> notified{0};
    EXPECT_EQ(nbasync::claim(state,
                             [&] {
                                 notified++;
                             }),
              0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(ranOn.load(), nullptr);
    pthread_t urger = nullptr;
    std::thread thread([&] {
        @autoreleasepool {
            urger = pthread_self();
            EXPECT_EQ(nbasync::urge(state), 1);
        }
    });
    thread.join();
    EXPECT_TRUE(pthread_equal(ranOn.load(), urger));
    EXPECT_EQ(notified.load(), 1u);
    EXPECT_EQ(nbasync::claim(state), 1);
}

TEST(AsyncPipelines, ProfileKeepsStatisticsOutOfRelease)
{
    char text[256] = "unchanged";
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    nbasync::counters().started++;
    EXPECT_GT(nbasync::appendRates(text, sizeof text), 0u);
    EXPECT_NE(strstr(text, "apso="), nullptr);
#else
    EXPECT_EQ(nbasync::appendRates(text, sizeof text), 0u);
    EXPECT_STREQ(text, "unchanged");
#endif
}
