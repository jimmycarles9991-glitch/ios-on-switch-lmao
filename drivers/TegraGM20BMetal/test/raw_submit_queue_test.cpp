// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <memory>
#include <vector>
#include "../raw/runtime/raw_submit_queue.h"

namespace {
using Queue = raw::SubmitQueue<int, int>;
using namespace std::chrono_literals;

void commit(Queue &q, int key, int value)
{
    const auto reservation = q.reserve(key, value);
    ASSERT_TRUE(q.commit(key, value, reservation));
}

TEST(SubmitQueue, ProducerBypassesParkedConsumer)
{
    Queue q;
    commit(q, 1, 10);
    commit(q, 1, 11);
    commit(q, 2, 20);
    auto consumer = q.next();
    ASSERT_EQ(consumer.value, 10);
    ASSERT_TRUE(q.park(consumer));
    auto producer = q.next();
    ASSERT_EQ(producer.value, 20);
    q.retire(producer);
    EXPECT_FALSE(q.next());
    q.wake(1);
    consumer = q.next();
    ASSERT_EQ(consumer.value, 10);
    q.retire(consumer);
    EXPECT_EQ(q.next().value, 11);
}

// What the submit worker tells raw_progress_gate.h before it sleeps: heads parked until their wake, their progress or
// their deadline
TEST(SubmitQueue, ParkedCountsHeadsUntilAWakeTheirProgressOrTheirDeadline)
{
    Queue q;
    commit(q, 1, 10);
    commit(q, 2, 20);
    commit(q, 3, 30);
    ASSERT_TRUE(q.park(q.next()));
    ASSERT_TRUE(q.park(q.next(), 1));
    const auto now = Queue::Time::clock::now();
    ASSERT_TRUE(q.park(q.next(), 0, now));
    EXPECT_EQ(q.parked(), 3u);
    q.wake(1);
    EXPECT_EQ(q.parked(), 2u);
    q.wakeDependencies(1);
    EXPECT_EQ(q.parked(), 1u);
    q.expire(now);
    EXPECT_EQ(q.parked(), 0u);
    auto t = q.next();
    q.wake(t.key);
    EXPECT_FALSE(q.park(t)) << "a head woken while judged is judged again, not parked";
    EXPECT_EQ(q.parked(), 0u);
}

TEST(SubmitQueue, EnqueueReservesOrderBeforeCommit)
{
    Queue q;
    const auto reservation = q.reserve(1, 1);
    commit(q, 1, 2);
    EXPECT_FALSE(q.next());
    q.commit(1, 1, reservation);
    auto t = q.next();
    ASSERT_EQ(t.value, 1);
    q.retire(t);
    EXPECT_EQ(q.next().value, 2);
}

TEST(SubmitQueue, WakeDuringReadinessCheckCannotBeLost)
{
    Queue q;
    commit(q, 1, 10);
    auto t = q.next();
    q.wake(1);
    EXPECT_FALSE(q.park(t));
    t = q.next();
    ASSERT_EQ(t.value, 10);
    ASSERT_TRUE(q.park(t));
    EXPECT_FALSE(q.next());
}

TEST(SubmitQueue, ProgressBeforeDependencyRegistrationCannotBeLost)
{
    Queue q;
    commit(q, 1, 10);
    auto t = q.next();
    q.wakeDependencies(1);
    EXPECT_FALSE(q.park(t, 1));
    t = q.next();
    q.wakeDependencies(2);
    EXPECT_TRUE(q.park(t, 1));
    EXPECT_FALSE(q.next());
    q.wakeDependencies(1);
    EXPECT_EQ(q.next().value, 10);
}

TEST(SubmitQueue, OnlyAffectedHeadRunsAmongTenThousandWaiters)
{
    Queue q;
    for (int key = 1; key <= 10000; ++key) {
        commit(q, key, key);
        auto t = q.next();
        ASSERT_EQ(t.key, key);
        q.park(t);
    }
    q.wake(7000);
    auto t = q.next();
    ASSERT_EQ(t.key, 7000);
    EXPECT_FALSE(q.next());
    q.retire(t);
    EXPECT_EQ(q.size(), 9999u);
}

TEST(SubmitQueue, YieldingHeadCannotStarveOtherQueues)
{
    Queue q;
    for (int key : {1, 2, 3})
        commit(q, key, key * 10);
    for (int cycle = 0; cycle < 10000; ++cycle)
        for (int key : {1, 2, 3}) {
            auto t = q.next();
            ASSERT_EQ(t.key, key);
            q.yield(t);
        }
}

TEST(SubmitQueue, OverlappingDependencyMasksQueueEachHeadOnce)
{
    Queue q;
    for (int key : {1, 2, 3})
        commit(q, key, key);
    auto a = q.next(), b = q.next(), c = q.next();
    q.park(a, 3);
    q.park(b, 1);
    q.park(c, 2);
    q.wakeDependencies(3);
    EXPECT_EQ(q.next().key, 1);
    EXPECT_EQ(q.next().key, 2);
    EXPECT_EQ(q.next().key, 3);
    EXPECT_FALSE(q.next());
}

TEST(SubmitQueue, DeadlinesWakeExactlyTheirHeadAndLeaveNoStaleTimers)
{
    Queue q;
    commit(q, 1, 10);
    commit(q, 2, 20);
    auto a = q.next(), b = q.next();
    q.park(a, 0, Queue::Time{} + 50ms);
    q.park(b, 0, Queue::Time{} + 60ms);
    q.expire(Queue::Time{} + 49ms);
    EXPECT_FALSE(q.next());
    q.expire(Queue::Time{} + 50ms);
    a = q.next();
    ASSERT_EQ(a.key, 1);
    q.retire(a);
    EXPECT_FALSE(q.next());
    q.wake(2);
    b = q.next();
    ASSERT_EQ(b.key, 2);
    EXPECT_EQ(q.deadline(), Queue::Time::max());
    q.retire(b);
}

TEST(SubmitQueue, RecycledNodesAndRepeatedValuesRejectStaleTickets)
{
    Queue q;
    commit(q, 1, 10);
    auto old = q.next();
    q.retire(old);
    commit(q, 2, 10);
    auto current = q.next();
    q.wake(1);
    EXPECT_FALSE(q.next());
    EXPECT_FALSE(q.retire(old));
    q.retire(current);
    commit(q, 1, 10);
    current = q.next();
    EXPECT_FALSE(q.retire(old));
    EXPECT_TRUE(q.retire(current));
}

TEST(SubmitQueue, TicketKeepsFinalOwnerAliveOutsideSchedulerLock)
{
    bool locked = false, destroyed = false;
    auto value = std::shared_ptr<int>(new int(1), [&](int *p) {
        EXPECT_FALSE(locked);
        destroyed = true;
        delete p;
    });
    raw::SubmitQueue<int, std::shared_ptr<int>> q;
    const auto reservation = q.reserve(1, value);
    q.commit(1, value, reservation);
    value.reset();
    auto ticket = q.next();
    locked = true;
    q.retire(ticket);
    EXPECT_FALSE(destroyed);
    locked = false;
    ticket.value.reset();
    EXPECT_TRUE(destroyed);
}

struct CountedValue {
    unsigned value = 0;
    size_t *comparisons = nullptr;

    bool operator!=(const CountedValue &other) const
    {
        ++*comparisons;
        return value != other.value;
    }
};

TEST(SubmitQueue, CommitsAddressReservationsWithoutScanningPendingWork)
{
    raw::SubmitQueue<int, CountedValue> queue;
    std::vector<decltype(queue)::Reservation> reservations;
    size_t comparisons = 0;
    for (unsigned i = 0; i < 10000; ++i)
        reservations.push_back(queue.reserve(1, {i, &comparisons}));
    for (unsigned i = 10000; i-- > 0;)
        ASSERT_TRUE(queue.commit(1, {i, &comparisons}, reservations[i]));
    EXPECT_EQ(comparisons, 10000u);
    EXPECT_EQ(queue.next().value.value, 0u);
}

TEST(SubmitQueue, ReservationsSurvivePrefixRetirementAndRejectRecycledQueues)
{
    Queue queue;
    const auto first = queue.reserve(1, 10);
    const auto second = queue.reserve(1, 20);
    ASSERT_TRUE(queue.commit(1, 10, first));
    ASSERT_TRUE(queue.retire(queue.next()));
    EXPECT_FALSE(queue.commit(1, 10, first));
    EXPECT_FALSE(queue.commit(1, 10, second));
    ASSERT_TRUE(queue.commit(1, 20, second));
    ASSERT_TRUE(queue.retire(queue.next()));
    const auto replacement = queue.reserve(2, 20);
    EXPECT_FALSE(queue.commit(2, 20, second));
    EXPECT_FALSE(queue.commit(2, 20, {}));
    EXPECT_FALSE(queue.commit(2, 20, {replacement.generation, UINT64_MAX}));
    ASSERT_TRUE(queue.commit(2, 20, replacement));
    ASSERT_TRUE(queue.retire(queue.next()));
    const auto reused = queue.reserve(1, 10);
    EXPECT_FALSE(queue.commit(1, 10, first));
    EXPECT_TRUE(queue.commit(1, 10, reused));
}

TEST(EventDeadline, AnUntimedWaitWaitsUntilItsValueAndNeverFails)
{
    using namespace std::chrono_literals;
    const raw::EventDeadline::Time start{};
    raw::EventDeadline wait(start, 0);
    EXPECT_EQ(wait.state(false, start + 9s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.state(false, start + 10s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.state(false, start + 3600s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.until(), raw::EventDeadline::Time::max());
    EXPECT_EQ(wait.state(true, start + 3600s), raw::EventReadiness::ready);
}

TEST(SubmitQueue, AHeldQueueCountsItsJobsScheduledAndThoseCommittedBehindIt)
{
    raw::SubmitQueue<int, int> queue;
    const auto head = queue.reserve(1, 10);
    const auto behind = queue.reserve(1, 11);
    const auto later = queue.reserve(1, 12);
    ASSERT_TRUE(queue.commit(1, 10, head));
    ASSERT_TRUE(queue.commit(1, 11, behind));
    auto ticket = queue.next();
    ASSERT_TRUE(ticket);
    EXPECT_TRUE(queue.park(ticket));
    std::vector<int> held;
    queue.hold(1, held);
    EXPECT_EQ(held, (std::vector<int>{10, 11}));
    held.clear();
    queue.hold(1, held);
    EXPECT_TRUE(held.empty());
    bool committedHeld = false;
    ASSERT_TRUE(queue.commit(1, 12, later, &committedHeld));
    EXPECT_TRUE(committedHeld);
    queue.wake(1);
    ticket = queue.next();
    ASSERT_TRUE(ticket);
    std::vector<int> released;
    queue.release(1, released);
    EXPECT_EQ(released, (std::vector<int>{10, 11, 12}));
    released.clear();
    queue.release(1, released);
    EXPECT_TRUE(released.empty());
    EXPECT_TRUE(queue.retire(ticket));
    const auto next = queue.reserve(1, 13);
    ASSERT_TRUE(queue.commit(1, 13, next, &committedHeld));
    EXPECT_FALSE(committedHeld);
}

// The stall line counts the scheduler's work: a reserved job its client has not committed yet is none
TEST(SubmitQueue, OnlyCommittedJobsArePending)
{
    raw::SubmitQueue<int, int> queue;
    const auto first = queue.reserve(1, 10);
    queue.reserve(1, 11);
    queue.reserve(2, 20);
    EXPECT_EQ(queue.size(), 3u);
    EXPECT_EQ(queue.committed(), 0u);
    ASSERT_TRUE(queue.commit(1, 10, first));
    EXPECT_EQ(queue.committed(), 1u);
    auto ticket = queue.next();
    ASSERT_TRUE(ticket);
    EXPECT_TRUE(queue.retire(ticket));
    EXPECT_EQ(queue.committed(), 0u);
}

TEST(SubmitQueue, AHoldOfOneQueueLeavesAnotherQueueUnheld)
{
    raw::SubmitQueue<int, int> queue;
    const auto a = queue.reserve(1, 10);
    const auto b = queue.reserve(2, 20);
    ASSERT_TRUE(queue.commit(1, 10, a));
    std::vector<int> held;
    queue.hold(1, held);
    bool committedHeld = true;
    ASSERT_TRUE(queue.commit(2, 20, b, &committedHeld));
    EXPECT_FALSE(committedHeld);
    std::vector<int> released;
    queue.release(2, released);
    EXPECT_TRUE(released.empty());
}

TEST(EventDeadline, ExplicitTimeoutRetainsItsRequestedDuration)
{
    using namespace std::chrono_literals;
    const raw::EventDeadline::Time start{};
    raw::EventDeadline wait(start, 30000);
    EXPECT_EQ(wait.state(false, start + 10s), raw::EventReadiness::blocked);
    EXPECT_EQ(wait.state(false, start + 30s), raw::EventReadiness::ready);
}

}  // namespace
