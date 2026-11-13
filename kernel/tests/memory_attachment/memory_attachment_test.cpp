// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include "memory_attachment_stubs.h"

static std::atomic<unsigned> admissionNotifications;

extern "C" void switch_gpu_admission_changed(void)
{
    ++admissionNotifications;
}

TEST(SwitchMemoryAttachment, LifetimeFollowsDescriptorAndRetainedCopies)
{
    unsigned freed = 0;
    auto *memory = new IOMemoryDescriptor;
    auto *object = new OSObject([&] {
        ++freed;
    });
    ASSERT_TRUE(SwitchMemoryAttachment::replace(memory, nullptr, object));
    object->release();
    auto *copy = SwitchMemoryAttachment::copy(memory);
    ASSERT_NE(copy, nullptr);
    delete memory;
    EXPECT_EQ(freed, 0u);
    copy->release();
    EXPECT_EQ(freed, 1u);
}

TEST(SwitchMemoryAttachment, FailedReplacementPreservesBothReferences)
{
    IOMemoryDescriptor memory;
    auto *first = new OSObject, *second = new OSObject;
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, first));
    EXPECT_FALSE(SwitchMemoryAttachment::replace(&memory, nullptr, second));
    EXPECT_EQ(first->refs, 2u);
    EXPECT_EQ(second->refs, 1u);
    EXPECT_TRUE(SwitchMemoryAttachment::replace(&memory, first, first));
    EXPECT_EQ(first->refs, 2u);
    EXPECT_TRUE(SwitchMemoryAttachment::replace(&memory, first, nullptr));
    EXPECT_EQ(first->refs, 1u);
    first->release();
    second->release();
}

TEST(SwitchMemoryAttachment, DisplacedDestructionRunsOutsideLock)
{
    IOMemoryDescriptor memory;
    bool destroyed = false;
    auto *first = new OSObject([&] {
        auto *copy = SwitchMemoryAttachment::copy(&memory);
        EXPECT_NE(copy, nullptr);
        if (copy)
            copy->release();
        destroyed = true;
    });
    auto *next = new OSObject;
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, first));
    first->release();
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, first, next));
    EXPECT_TRUE(destroyed);
    next->release();
}

TEST(SwitchMemoryAttachment, ConcurrentReadersAndReplacementsRetireExactlyOnce)
{
    std::atomic<unsigned> made{0}, freed{0};
    {
        IOMemoryDescriptor memory;
        std::vector<std::thread> workers;
        for (unsigned t = 0; t != 4; ++t)
            workers.emplace_back([&] {
                for (unsigned i = 0; i != 10000; ++i) {
                    auto *old = SwitchMemoryAttachment::copy(&memory);
                    auto *next = new OSObject([&] {
                        ++freed;
                    });
                    ++made;
                    SwitchMemoryAttachment::replace(&memory, old, next);
                    next->release();
                    if (old)
                        old->release();
                }
            });
        for (auto &worker : workers)
            worker.join();
    }
    EXPECT_EQ(made, 40000u);
    EXPECT_EQ(freed, made.load());
}

TEST(SwitchMemoryAttachment, AllocationFailureCanRetryWithoutChangingObjectOwnership)
{
    IOMemoryDescriptor memory;
    auto *object = new OSObject;
    switchAllocationFailures = 1;
    EXPECT_FALSE(SwitchMemoryAttachment::replace(&memory, nullptr, object));
    EXPECT_EQ(object->refs, 1u);
    EXPECT_EQ(SwitchMemoryAttachment::copy(&memory), nullptr);
    EXPECT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, object));
    EXPECT_EQ(object->refs, 2u);
    object->release();
}

TEST(SwitchMemoryAttachment, FirstPublicationPreservesAllConcurrentDescriptorUsers)
{
    IOMemoryDescriptor memory;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t != 16; ++t)
        workers.emplace_back([&] {
            for (unsigned i = 0; i != 10000; ++i) {
                auto *reserved = SwitchMemoryAttachment::prepare(&memory);
                ASSERT_NE(reserved, nullptr);
                ++reserved->unrelatedState;
            }
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(SwitchMemoryAttachment::prepare(&memory)->unrelatedState, 160000u);
}

TEST(SwitchMemoryAttachment, CleanupDropsAuxiliaryStateWithoutTouchingUnrelatedState)
{
    IOMemoryDescriptor memory;
    auto *reserved = SwitchMemoryAttachment::prepare(&memory);
    reserved->unrelatedState = 42;
    bool destroyed = false;
    auto *object = new OSObject([&] {
        EXPECT_EQ(SwitchMemoryAttachment::copy(&memory), nullptr);
        destroyed = true;
    });
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, object));
    object->release();
    SwitchMemoryAttachment::clear(reserved);
    EXPECT_TRUE(destroyed);
    EXPECT_EQ(reserved->unrelatedState, 42u);
    SwitchMemoryAttachment::clear(reserved);
}

namespace {
unsigned sleepCount()
{
    std::lock_guard held(switchSleepMutex);
    return switchSleeps;
}

bool awaitSleep(unsigned previous)
{
    std::unique_lock held(switchSleepMutex);
    return switchSleepChanged.wait_for(held, std::chrono::seconds(5), [&] {
        return switchSleeps > previous;
    });
}
}

TEST(SwitchMemoryAttachment, FirstPublisherCannotPassAnUnimportedCpuAlias)
{
    IOMemoryDescriptor memory;
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), 0);
    EXPECT_EQ(memory.refs, 2u);
    EXPECT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, true), -16);
    EXPECT_EQ(memory.refs, 2u);
    EXPECT_EQ(SwitchMemoryAttachment::copy(&memory), nullptr);
    SwitchMemoryAttachment::leaveCpu(&memory);
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, true), 0);
    EXPECT_EQ(memory.refs, 2u);
    SwitchMemoryAttachment::leaveExclusive(&memory);
    EXPECT_EQ(memory.refs, 1u);
}

TEST(SwitchMemoryAttachment, WaitingPublisherDoesNotDeadlockNestedCpuLocks)
{
    IOMemoryDescriptor memory;
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), 0);
    const auto sleeps = sleepCount();
    std::atomic<bool> published{false};
    std::thread publisher([&] {
        EXPECT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, false), 0);
        published = true;
        SwitchMemoryAttachment::leaveExclusive(&memory);
    });
    EXPECT_TRUE(awaitSleep(sleeps));
    EXPECT_FALSE(published);
    const int nested = SwitchMemoryAttachment::enterCpu(&memory, true);
    EXPECT_EQ(nested, 0);
    if (!nested)
        SwitchMemoryAttachment::leaveCpu(&memory);
    EXPECT_FALSE(published);
    SwitchMemoryAttachment::leaveCpu(&memory);
    publisher.join();
    EXPECT_TRUE(published);
    EXPECT_EQ(memory.refs, 1u);
}

TEST(SwitchMemoryAttachment, ExclusivePublicationExcludesCpuAndOtherPublishers)
{
    IOMemoryDescriptor memory;
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, false), 0);
    EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, true), -16);
    EXPECT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, true), -16);
    EXPECT_EQ(memory.refs, 2u);
    SwitchMemoryAttachment::leaveExclusive(&memory);
    EXPECT_EQ(memory.refs, 1u);
}

TEST(SwitchMemoryAttachment, CpuWaiterResumesAfterPublicationAndObservesPayload)
{
    IOMemoryDescriptor memory;
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, false), 0);
    const auto sleeps = sleepCount();
    auto *object = new OSObject;
    std::thread reader([&] {
        EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), 0);
        auto *observed = SwitchMemoryAttachment::copy(&memory);
        EXPECT_EQ(observed, object);
        if (observed)
            observed->release();
        SwitchMemoryAttachment::leaveCpu(&memory);
    });
    EXPECT_TRUE(awaitSleep(sleeps));
    EXPECT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, object));
    SwitchMemoryAttachment::leaveExclusive(&memory);
    reader.join();
    EXPECT_EQ(memory.refs, 1u);
    object->release();
}

TEST(SwitchMemoryAttachment, DescriptorDestructionWaitsForLastCpuReservation)
{
    bool destroyed = false;
    auto *memory = new IOMemoryDescriptor;
    auto *object = new OSObject([&] {
        destroyed = true;
    });
    ASSERT_TRUE(SwitchMemoryAttachment::replace(memory, nullptr, object));
    object->release();
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(memory, false), 0);
    memory->release();
    EXPECT_FALSE(destroyed);
    SwitchMemoryAttachment::leaveCpu(memory);
    EXPECT_TRUE(destroyed);
}

TEST(SwitchMemoryAttachment, ExclusiveReleaseCanDestroyDescriptorAndReenterAttachmentCode)
{
    auto *memory = new IOMemoryDescriptor;
    IOMemoryDescriptor other;
    bool destroyed = false;
    auto *object = new OSObject([&] {
        EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&other, true), 0);
        SwitchMemoryAttachment::leaveCpu(&other);
        destroyed = true;
    });
    ASSERT_TRUE(SwitchMemoryAttachment::replace(memory, nullptr, object));
    object->release();
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(memory, false), 0);
    memory->release();
    EXPECT_FALSE(destroyed);
    SwitchMemoryAttachment::leaveExclusive(memory);
    EXPECT_TRUE(destroyed);
}

TEST(SwitchMemoryAttachment, ReservationAllocationFailureLeavesDescriptorUnretained)
{
    IOMemoryDescriptor memory;
    switchAllocationFailures = 1;
    EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), -12);
    EXPECT_EQ(memory.refs, 1u);
    switchAllocationFailures = 1;
    EXPECT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, false), -12);
    EXPECT_EQ(memory.refs, 1u);
    ASSERT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), 0);
    SwitchMemoryAttachment::leaveCpu(&memory);
}

TEST(SwitchMemoryAttachment, ConcurrentReadersAndPublishersNeverOverlap)
{
    IOMemoryDescriptor memory;
    std::atomic<unsigned> readers{0}, writers{0};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index != 4; ++index)
        workers.emplace_back([&, index] {
            for (unsigned iteration = 0; iteration != 2000; ++iteration) {
                if (index == 0) {
                    EXPECT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, false), 0);
                    EXPECT_EQ(++writers, 1u);
                    EXPECT_EQ(readers, 0u);
                    --writers;
                    SwitchMemoryAttachment::leaveExclusive(&memory);
                } else {
                    EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, false), 0);
                    ++readers;
                    EXPECT_EQ(writers, 0u);
                    --readers;
                    SwitchMemoryAttachment::leaveCpu(&memory);
                }
            }
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(memory.refs, 1u);
}

TEST(SwitchMemoryAttachment, RepresentationStampSurvivesPayloadReplacementAndRemoval)
{
    IOMemoryDescriptor memory;
    auto *stamp = SwitchMemoryAttachment::generation(&memory);
    ASSERT_NE(stamp, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(stamp) & 7, 0u);
    EXPECT_EQ(__atomic_load_n(stamp, __ATOMIC_ACQUIRE), 1u);
    auto *payload = new OSObject;
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, nullptr, payload));
    __atomic_store_n(stamp, uint64_t(2), __ATOMIC_RELEASE);
    EXPECT_EQ(SwitchMemoryAttachment::generation(&memory), stamp);
    ASSERT_TRUE(SwitchMemoryAttachment::replace(&memory, payload, nullptr));
    EXPECT_EQ(__atomic_load_n(SwitchMemoryAttachment::generation(&memory), __ATOMIC_ACQUIRE), 2u);
    payload->release();
}

TEST(SwitchMemoryAttachment, ConcurrentStampInitializationCannotResetAChangedGeneration)
{
    IOMemoryDescriptor memory;
    std::vector<std::thread> workers;
    for (unsigned t = 0; t != 8; ++t)
        workers.emplace_back([&] {
            for (unsigned i = 0; i != 10000; ++i) {
                auto *stamp = SwitchMemoryAttachment::generation(&memory);
                ASSERT_NE(stamp, nullptr);
                __atomic_fetch_add(stamp, uint64_t(1), __ATOMIC_RELAXED);
            }
        });
    for (auto &worker : workers)
        worker.join();
    EXPECT_EQ(__atomic_load_n(SwitchMemoryAttachment::generation(&memory), __ATOMIC_ACQUIRE), 80001u);
}

TEST(SwitchMemoryAttachment, ExclusiveReleaseNotifiesAdmissionAfterUnlock)
{
    IOMemoryDescriptor memory;
    const auto before = admissionNotifications.load();
    ASSERT_EQ(SwitchMemoryAttachment::enterExclusive(&memory, true), 0);
    EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, true), -16);
    SwitchMemoryAttachment::leaveExclusive(&memory);
    EXPECT_EQ(admissionNotifications.load(), before + 1);
    EXPECT_EQ(SwitchMemoryAttachment::enterCpu(&memory, true), 0);
    SwitchMemoryAttachment::leaveCpu(&memory);
}

TEST(SwitchMemoryAttachment, GpuRecordHasItsOwnSlotAndEndsWithTheDescriptor)
{
    unsigned freed = 0;
    auto *memory = new IOMemoryDescriptor;
    auto *representation = new OSObject;
    auto *record = new OSObject([&] {
        EXPECT_EQ(SwitchMemoryAttachment::copyGpu(memory), nullptr);
        ++freed;
    });
    ASSERT_TRUE(SwitchMemoryAttachment::replace(memory, nullptr, representation));
    ASSERT_TRUE(SwitchMemoryAttachment::replaceGpu(memory, nullptr, record));
    EXPECT_FALSE(SwitchMemoryAttachment::replaceGpu(memory, nullptr, representation));
    record->release();
    auto *copy = SwitchMemoryAttachment::copyGpu(memory);
    EXPECT_EQ(copy, record);
    copy->release();
    auto *other = SwitchMemoryAttachment::copy(memory);
    EXPECT_EQ(other, representation);
    other->release();
    EXPECT_EQ(representation->refs, 2u);
    delete memory;
    EXPECT_EQ(freed, 1u);
    EXPECT_EQ(representation->refs, 1u);
    representation->release();
}
