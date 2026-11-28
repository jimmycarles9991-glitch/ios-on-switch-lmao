// SPDX-License-Identifier: MIT
// The process's command queues' GPU priorities on the device's connection (QueuePriority.h): every queue counts from its
// creation at Metal's defaults, and the kernel gets the most urgent of each whenever that changes. The connection is NVK's
// (gm20b::Kernel::shared), whose m2v queues keep their own count at the defaults and never send one: the twins this device
// makes never take a priority of their own.
#import "TegraGM20BMetalInternal.h"
#include "QueuePriority.h"

namespace {

struct QueuePriorityState {
    gm20b::UnfairMutex lock;
    switch_metal::QueuePriorities priorities;
    uint32_t applied = switch_metal::QueuePriorities::kDefault;
    uint32_t appliedBackground = switch_metal::QueuePriorities::kDefaultBackground;
};

// Never destroyed: the detached completion thread may release a command buffer's last reference to its queue while
// the process exits, after static destructors ran.
QueuePriorityState &priorityState()
{
    static QueuePriorityState *s = new QueuePriorityState;
    return *s;
}

void apply(QueuePriorityState &s)
{
    const uint32_t priority = s.priorities.priority(), background = s.priorities.backgroundPriority();
    if (priority == s.applied && background == s.appliedBackground)
        return;
    if (!gRM || !gRM->raw->gpuPriority(priority, background)) {
        SWITCH_REFUSE("gm20b", "queue.priority", "the kernel did not take the queues' GPU priority");
        return;
    }
    s.applied = priority;
    s.appliedBackground = background;
}

}  // namespace

void queuePriorityAdd(SwitchMetalQueue *queue)
{
    QueuePriorityState &s = priorityState();
    std::lock_guard<gm20b::UnfairMutex> l(s.lock);
    queue->_gpuPriority = switch_metal::QueuePriorities::kDefault;
    queue->_backgroundGpuPriority = switch_metal::QueuePriorities::kDefaultBackground;
    s.priorities.add(queue->_gpuPriority, queue->_backgroundGpuPriority);
}

void queuePriorityRemove(SwitchMetalQueue *queue)
{
    QueuePriorityState &s = priorityState();
    std::lock_guard<gm20b::UnfairMutex> l(s.lock);
    s.priorities.remove(queue->_gpuPriority, queue->_backgroundGpuPriority);
    apply(s);
}

// NO for a value Metal does not have, or on a kernel without the call (as m2v's queue answers)
static BOOL setPriority(SwitchMetalQueue *queue, NSUInteger priority, bool background)
{
    if (priority > switch_metal::QueuePriorities::kMax || !gRM || !gRM->raw->hasGpuPriority())
        return NO;
    QueuePriorityState &s = priorityState();
    std::lock_guard<gm20b::UnfairMutex> l(s.lock);
    s.priorities.remove(queue->_gpuPriority, queue->_backgroundGpuPriority);
    (background ? queue->_backgroundGpuPriority : queue->_gpuPriority) = uint32_t(priority);
    s.priorities.add(queue->_gpuPriority, queue->_backgroundGpuPriority);
    apply(s);
    return YES;
}

@interface SwitchMetalQueue (Priority)
@end
@implementation SwitchMetalQueue (Priority)

- (BOOL)setGPUPriority:(NSUInteger)priority
{
    return setPriority(self, priority, false);
}

- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority
{
    return setPriority(self, priority, true);
}

- (NSUInteger)getGPUPriority
{
    QueuePriorityState &s = priorityState();
    std::lock_guard<gm20b::UnfairMutex> l(s.lock);
    return _gpuPriority;
}

- (NSUInteger)getBackgroundGPUPriority
{
    QueuePriorityState &s = priorityState();
    std::lock_guard<gm20b::UnfairMutex> l(s.lock);
    return _backgroundGpuPriority;
}

// Submission is the device's own submit thread (TegraGM20BMetalSubmit.mm), in commit order
- (void)setSubmissionQueue:(dispatch_queue_t)queue
{
    (void)queue;
}

@end
