// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
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
