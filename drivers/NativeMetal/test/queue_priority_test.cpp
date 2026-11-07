// SPDX-License-Identifier: MIT
// QueuePriority.h: the GPU priority the GPU driver gets for a process, from its Metal queues', as Core Animation sets them.
#include <gtest/gtest.h>
#include "drivers/NativeMetal/QueuePriority.h"

namespace {
using switch_metal::QueuePriorities;

TEST(QueuePriority, NoQueueOrNoCallIsTheDefault)
{
    QueuePriorities q;
    EXPECT_EQ(q.priority(), QueuePriorities::kDefault);
    EXPECT_EQ(q.backgroundPriority(), QueuePriorities::kDefaultBackground);
    q.add(QueuePriorities::kDefault, QueuePriorities::kDefaultBackground);
    EXPECT_EQ(q.priority(), 2u);
}

// backboardd: the window server renderer (0) and the asynchronous dispatcher (1) in one process, set in either order
TEST(QueuePriority, TheRenderServerKeepsItsMostUrgentQueue)
{
    QueuePriorities q;
    q.add(0, QueuePriorities::kDefaultBackground);
    q.add(1, QueuePriorities::kDefaultBackground);
    EXPECT_EQ(q.priority(), 0u);
    q.remove(0, QueuePriorities::kDefaultBackground);
    EXPECT_EQ(q.priority(), 1u);
    q.remove(1, QueuePriorities::kDefaultBackground);
    EXPECT_EQ(q.priority(), QueuePriorities::kDefault);
}

// an app's background context (setBackgroundGPUPriority: 2) keeps it at the normal priority in the background
TEST(QueuePriority, BackgroundPriorityIsCombinedOnItsOwn)
{
    QueuePriorities q;
    q.add(2, QueuePriorities::kDefaultBackground);
    q.add(4, 2);
    EXPECT_EQ(q.priority(), 2u);
    EXPECT_EQ(q.backgroundPriority(), 2u);
}
}  // namespace
