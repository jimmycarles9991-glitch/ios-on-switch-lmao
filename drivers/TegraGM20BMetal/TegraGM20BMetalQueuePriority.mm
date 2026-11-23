// SPDX-License-Identifier: MIT
// The process's command queues' GPU priorities on the device's connection (QueuePriority.h): every queue counts from its
// creation at Metal's defaults, and the kernel gets the most urgent of each whenever that changes. The connection is NVK's
// (gm20b::Kernel::shared), whose m2v queues keep their own count at the defaults and never send one: the twins this device
// makes never take a priority of their own.
#import "TegraGM20BMetalInternal.h"
#include "QueuePriority.h"

namespace {

gm20b::Mutex gLock;
switch_metal::QueuePriorities gPriorities;
uint32_t gApplied = switch_metal::QueuePriorities::kDefault,
         gAppliedBackground = switch_metal::QueuePriorities::kDefaultBackground;

// gLock held
void apply()
{
    const uint32_t priority = gPriorities.priority(), background = gPriorities.backgroundPriority();
    if (priority == gApplied && background == gAppliedBackground)
        return;
    if (!gRM || !gRM->raw->gpuPriority(priority, background)) {
        SWITCH_REFUSE("gm20b", "queue.priority", "the kernel did not take the queues' GPU priority");
        return;
    }
    gApplied = priority;
    gAppliedBackground = background;
}

}  // namespace

void queuePriorityAdd(SwitchMetalQueue *queue)
{
    std::lock_guard<gm20b::Mutex> l(gLock);
    queue->_gpuPriority = switch_metal::QueuePriorities::kDefault;
    queue->_backgroundGpuPriority = switch_metal::QueuePriorities::kDefaultBackground;
    gPriorities.add(queue->_gpuPriority, queue->_backgroundGpuPriority);
}

void queuePriorityRemove(SwitchMetalQueue *queue)
{
    std::lock_guard<gm20b::Mutex> l(gLock);
    gPriorities.remove(queue->_gpuPriority, queue->_backgroundGpuPriority);
    apply();
}

// NO for a value Metal does not have, or on a kernel without the call (as m2v's queue answers)
static BOOL setPriority(SwitchMetalQueue *queue, NSUInteger priority, bool background)
{
    if (priority > switch_metal::QueuePriorities::kMax || !gRM || !gRM->raw->hasGpuPriority())
        return NO;
    std::lock_guard<gm20b::Mutex> l(gLock);
    gPriorities.remove(queue->_gpuPriority, queue->_backgroundGpuPriority);
    (background ? queue->_backgroundGpuPriority : queue->_gpuPriority) = uint32_t(priority);
    gPriorities.add(queue->_gpuPriority, queue->_backgroundGpuPriority);
    apply();
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
    std::lock_guard<gm20b::Mutex> l(gLock);
    return _gpuPriority;
}

- (NSUInteger)getBackgroundGPUPriority
{
    std::lock_guard<gm20b::Mutex> l(gLock);
    return _backgroundGpuPriority;
}

// Submission is the device's own submit thread (TegraGM20BMetalSubmit.mm), in commit order
- (void)setSubmissionQueue:(dispatch_queue_t)queue
{
    (void)queue;
}

@end
