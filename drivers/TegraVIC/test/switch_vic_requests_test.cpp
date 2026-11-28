// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <vector>
#include "drivers/TegraVIC/SwitchVICRequests.h"

namespace {
struct Conn {
    uint32_t waiting = 0;
};

struct Request {
    Request *next = nullptr;
    Conn *conn = nullptr;
    bool eventReady = false, listed = false;
    int eventStatus = 0;
    const void *touched[2] = {};
    bool reserving = false;
};

using Requests = SwitchVICRequests<Request>;

std::vector<Request> make(Conn *conn, uint32_t count, bool ready)
{
    std::vector<Request> requests(count);
    for (auto &request : requests) {
        request.conn = conn;
        request.eventReady = ready;
    }
    return requests;
}
}  // namespace

TEST(SwitchVICRequests, ClientWaitingOnEventsThatNeverFireStallsOnlyItself)
{
    Requests queue;
    Conn stuck, other;
    auto waiting = make(&stuck, Requests::kWaitingPerConnection + 1, false);
    for (uint32_t i = 0; i < Requests::kWaitingPerConnection; ++i) {
        ASSERT_TRUE(queue.admits(waiting[i]));
        queue.push(&waiting[i]);
    }
    EXPECT_FALSE(queue.admits(waiting.back()));
    auto runnable = make(&other, Requests::kRunnable, true);
    for (auto &request : runnable) {
        ASSERT_TRUE(queue.admits(request));
        queue.push(&request);
    }
    for (auto &request : runnable)
        EXPECT_EQ(queue.pop(), &request);
    EXPECT_EQ(queue.pop(), nullptr);
    auto otherWaiting = make(&other, 1, false);
    EXPECT_TRUE(queue.admits(otherWaiting[0]));
}

TEST(SwitchVICRequests, FiredEventsMoveARequestToTheRunnableBound)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, 2, false);
    queue.push(&requests[0]);
    queue.push(&requests[1]);
    EXPECT_EQ(conn.waiting, 2u);
    queue.ready(&requests[1], 0);
    EXPECT_EQ(conn.waiting, 1u);
    EXPECT_EQ(queue.pop(), &requests[1]);
    EXPECT_EQ(queue.pop(), nullptr);
    queue.ready(&requests[0], -5);
    EXPECT_EQ(conn.waiting, 0u);
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_EQ(requests[0].eventStatus, -5);
    EXPECT_FALSE(requests[0].listed);
}

TEST(SwitchVICRequests, EventsFiringBeforeTheRequestIsListedOnlyMarkIt)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, 1, false);
    queue.ready(&requests[0], 0);
    EXPECT_EQ(conn.waiting, 0u);
    queue.push(&requests[0]);
    EXPECT_EQ(conn.waiting, 0u);
    EXPECT_EQ(queue.pop(), &requests[0]);
}

TEST(SwitchVICRequests, CloseFailsEveryQueuedRequestOfItsConnectionOnly)
{
    Requests queue;
    Conn closing, other;
    auto mine = make(&closing, 3, false);
    mine[2].eventReady = true;
    auto theirs = make(&other, 1, false);
    for (auto &request : mine)
        queue.push(&request);
    queue.push(&theirs[0]);
    queue.cancel(&closing, -125);
    EXPECT_EQ(closing.waiting, 0u);
    EXPECT_EQ(other.waiting, 1u);
    for (auto &request : mine) {
        EXPECT_EQ(queue.pop(), &request);
        EXPECT_EQ(request.eventStatus, -125);
    }
    EXPECT_EQ(queue.pop(), nullptr);
    EXPECT_EQ(theirs[0].eventStatus, 0);
}

TEST(SwitchVICRequests, RunnableBoundCountsOnlyQueuedRunnableRequests)
{
    Requests queue;
    Conn conn;
    auto requests = make(&conn, Requests::kRunnable + 1, true);
    for (uint32_t i = 0; i < Requests::kRunnable; ++i)
        queue.push(&requests[i]);
    EXPECT_FALSE(queue.admits(requests.back()));
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_TRUE(queue.admits(requests.back()));
}

TEST(SwitchVICRequests, ASurfaceIsAloneOnlyWhenNoListedOrRunningRequestTouchesIt)
{
    Requests queue;
    Conn conn;
    int source = 0, destination = 0, other = 0;
    auto requests = make(&conn, 1, true);
    requests[0].touched[0] = &destination;
    requests[0].touched[1] = &source;
    EXPECT_TRUE(queue.alone(&destination));
    queue.push(&requests[0]);
    EXPECT_FALSE(queue.alone(&destination));
    EXPECT_FALSE(queue.alone(&source));
    EXPECT_TRUE(queue.alone(&other));
    EXPECT_EQ(queue.pop(), &requests[0]);
    EXPECT_FALSE(queue.alone(&destination)) << "the engine thread runs it";
    queue.finished();
    EXPECT_TRUE(queue.alone(&destination));
    EXPECT_TRUE(queue.alone(&source));
}

TEST(SwitchVICRequests, NothingTouchingItsSurfacesOvertakesARequestTakingItsReservation)
{
    Requests queue;
    Conn conn;
    int source = 0, destination = 0, elsewhere = 0;
    auto requests = make(&conn, 4, true);
    requests[0].touched[0] = &destination;
    requests[0].touched[1] = &source;
    requests[0].reserving = true;
    requests[1].touched[0] = &destination;
    requests[2].touched[0] = &elsewhere;
    requests[2].touched[1] = &destination;
    requests[3].touched[0] = &source;
    for (auto &request : requests)
        queue.push(&request);
    EXPECT_EQ(queue.pop(), nullptr) << "each later request writes the destination, reads it, or writes the source";
    queue.reserved(&requests[0]);
    for (auto &request : requests)
        EXPECT_EQ(queue.pop(), &request);
}

// Y, held back behind R for reading R's destination, holds back X on Y's own destination in turn.
TEST(SwitchVICRequests, ARequestHeldBackBehindAReservingOneHoldsBackLaterRequestsOnItsSurfaces)
{
    Requests queue;
    Conn conn;
    int shared = 0, destination = 0;
    auto requests = make(&conn, 3, true);
    Request &r = requests[0], &y = requests[1], &x = requests[2];
    r.touched[0] = &shared;
    r.reserving = true;
    y.touched[0] = &destination;
    y.touched[1] = &shared;
    x.touched[0] = &destination;
    for (auto &request : requests)
        queue.push(&request);
    EXPECT_EQ(queue.pop(), nullptr) << "X wrote Y's destination before Y";
    queue.reserved(&r);
    EXPECT_EQ(queue.pop(), &r);
    EXPECT_EQ(queue.pop(), &y);
    EXPECT_EQ(queue.pop(), &x);
}

// A background client's job is still taking its reservation (its thread preempted, say): a compositor's job on other
// surfaces queued behind it runs meanwhile, and one on its destination still waits for it.
TEST(SwitchVICRequests, AnUnrelatedRequestRunsWhileAnotherIsStillTakingItsReservation)
{
    Requests queue;
    Conn background, compositor;
    int destination = 0, source = 0, frame = 0, layer = 0;
    auto reserving = make(&background, 1, true);
    reserving[0].touched[0] = &destination;
    reserving[0].touched[1] = &source;
    reserving[0].reserving = true;
    auto later = make(&compositor, 2, true);
    later[0].touched[0] = &destination;
    later[1].touched[0] = &frame;
    later[1].touched[1] = &layer;
    queue.push(&reserving[0]);
    for (auto &request : later)
        queue.push(&request);
    EXPECT_EQ(queue.pop(), &later[1]);
    queue.finished();
    EXPECT_EQ(queue.pop(), nullptr);
    queue.reserved(&reserving[0]);
    EXPECT_EQ(queue.pop(), &reserving[0]);
    EXPECT_EQ(queue.pop(), &later[0]);
}

namespace {
// A destination's write reservation as switch_gpu_backing_conversion_reserve grants it, to one holder at a time.
struct Destination {
    std::mutex lock;
    std::condition_variable changed;
    const void *holder = nullptr;

    bool tryReserve(const void *who)
    {
        std::lock_guard held(lock);
        if (holder)
            return false;
        holder = who;
        return true;
    }

    void reserve(const void *who)
    {
        std::unique_lock held(lock);
        changed.wait(held, [&] {
            return !holder;
        });
        holder = who;
    }

    bool heldBy(const void *who)
    {
        std::lock_guard held(lock);
        return holder == who;
    }

    void release(const void *who)
    {
        std::lock_guard held(lock);
        if (holder == who) {
            holder = nullptr;
            changed.notify_all();
        }
    }
};

// TegraVICUserClient's queue and engine thread around SwitchVICRequests: queueJob decides under the queue's lock and
// reserves outside it; the engine thread pops, runs, and marks the run over.
struct Engine {
    Requests queue;
    std::mutex lock;
    std::condition_variable changed;
    Destination destination;
    std::vector<const Request *> ran;
    std::function<void(const Request &)> popped;
    std::function<void(const Request &)> beforeReserving;

    void queueJob(Request *request, bool eventFree)
    {
        std::unique_lock held(lock);
        const bool reserve = eventFree && queue.alone(request->touched[0]);
        request->reserving = reserve;
        queue.push(request);
        changed.notify_all();
        held.unlock();
        if (!reserve)
            return;
        if (beforeReserving)
            beforeReserving(*request);
        destination.tryReserve(request);
        held.lock();
        queue.reserved(request);
        changed.notify_all();
    }

    void ready(Request *request)
    {
        std::lock_guard held(lock);
        queue.ready(request, 0);
        changed.notify_all();
    }

    // A job's write of the destination: under the reservation it took when queued, else one it waits for (a record's
    // install takes one that way).
    void run(const Request &request)
    {
        if (popped)
            popped(request);
        if (!destination.heldBy(&request))
            destination.reserve(&request);
        destination.release(&request);
    }

    void serve(size_t jobs)
    {
        while (ran.size() < jobs) {
            std::unique_lock held(lock);
            Request *request = nullptr;
            changed.wait(held, [&] {
                return (request = queue.pop()) != nullptr;
            });
            held.unlock();
            run(*request);
            held.lock();
            queue.finished();
            ran.push_back(request);
        }
    }
};

// Runs the engine thread for `jobs` jobs; false if they were not all done in 5 s (the destination is then freed so
// the thread can end).
bool served(Engine &engine, size_t jobs, const Request &stuck)
{
    auto done = std::async(std::launch::async, [&] {
        engine.serve(jobs);
    });
    const bool ended = done.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    if (!ended)
        engine.destination.release(&stuck);
    done.get();
    return ended;
}
}  // namespace

// Job A (with events) and then job B (without) write the same destination; A's events fire before the engine thread
// pops, so A runs first, and its record's install waits for the destination's reservation. B must not be holding it
// from when it was queued: B cannot run, and so release it, until A is done.
TEST(SwitchVICRequests, AJobQueuedBehindAnotherOnItsDestinationDoesNotStallIt)
{
    Engine engine;
    Conn conn;
    auto requests = make(&conn, 2, false);
    Request &a = requests[0], &b = requests[1];
    a.touched[0] = b.touched[0] = &engine.destination;
    engine.queueJob(&a, false);
    b.eventReady = true;
    engine.queueJob(&b, true);
    engine.ready(&a);
    EXPECT_TRUE(served(engine, 2, b)) << "A waited on B's reservation and B waited behind A";
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&a, &b}));
}

// The same with A already popped when B is queued: the engine thread's own job counts as touching the destination.
TEST(SwitchVICRequests, AJobQueuedWhileAnotherRunsOnItsDestinationDoesNotStallIt)
{
    Engine engine;
    Conn conn;
    auto requests = make(&conn, 2, true);
    Request &a = requests[0], &b = requests[1];
    a.touched[0] = b.touched[0] = &engine.destination;
    engine.popped = [&](const Request &request) {
        if (&request == &a)
            engine.queueJob(&b, true);
    };
    engine.queueJob(&a, false);
    EXPECT_TRUE(served(engine, 2, b)) << "A waited on B's reservation and B waited behind A";
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&a, &b}));
}

// B's thread is preempted between queueing B and taking B's reservation; C, on surfaces of its own, is queued behind B
// by another client and must run meanwhile.
TEST(SwitchVICRequests, AJobOnOtherSurfacesRunsWhileAnEarlierOneIsTakingItsReservation)
{
    Engine engine;
    Conn background, compositor;
    int frame = 0;
    auto requests = make(&background, 1, true);
    auto others = make(&compositor, 1, true);
    Request &b = requests[0], &c = others[0];
    b.touched[0] = &engine.destination;
    c.touched[0] = &frame;
    std::promise<void> resume, queued;
    auto resumed = resume.get_future();
    engine.beforeReserving = [&](const Request &) {
        queued.set_value();
        resumed.wait();
    };
    std::thread client([&] {
        engine.queueJob(&b, true);
    });
    queued.get_future().wait();
    engine.queueJob(&c, false);
    auto served = std::async(std::launch::async, [&] {
        engine.serve(1);
    });
    const bool ran = served.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    resume.set_value();
    client.join();
    served.get();
    EXPECT_TRUE(ran) << "C waited for B's thread to take B's reservation";
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&c}));
    engine.serve(2);
    EXPECT_EQ(engine.ran, (std::vector<const Request *>{&c, &b}));
}

TEST(SwitchVICRequests, ListedWaitingRequestExcludesOnlyItselfFromAlone)
{
    Requests queue;
    Conn conn;
    int destination = 0, other = 0;
    auto requests = make(&conn, 2, false);
    requests[0].touched[0] = &destination;
    requests[1].touched[0] = &destination;
    queue.push(&requests[0]);
    EXPECT_TRUE(queue.alone(&destination, &requests[0]));
    EXPECT_FALSE(queue.alone(&destination));
    queue.push(&requests[1]);
    EXPECT_FALSE(queue.alone(&destination, &requests[0]));
    EXPECT_FALSE(queue.alone(&destination, &requests[1]));
    EXPECT_TRUE(queue.alone(&other, &requests[0]));
}
