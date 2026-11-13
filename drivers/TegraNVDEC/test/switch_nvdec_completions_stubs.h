// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel SwitchNVDECCompletions.cpp calls, on the host: IOLock as a flag, IOUserClient's async-result statics
// recording what they were given.
#pragma once
#include <gtest/gtest.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <vector>
#include <mach/port.h>

using IOReturn = int32_t;
using UInt32 = uint32_t;
using io_user_reference_t = uint64_t;
using task_t = void *;
using IOOptionBits = uint32_t;
using OSAsyncReference64 = io_user_reference_t[8];
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = int32_t(0xe00002c2),
                   kIOReturnIOError = int32_t(0xe00002ca);

struct IOLock {
    bool locked = false;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockFree(IOLock *lock)
{
    EXPECT_FALSE(lock->locked);
    delete lock;
}

inline void IOLockLock(IOLock *lock)
{
    EXPECT_FALSE(lock->locked);
    lock->locked = true;
}

inline void IOLockUnlock(IOLock *lock)
{
    EXPECT_TRUE(lock->locked);
    lock->locked = false;
}

#define IONewZero(type, count)         static_cast<type *>(std::calloc((count), sizeof(type)))
#define IODelete(pointer, type, count) ((void)(count), std::free(pointer))

struct SwitchTestSent {
    mach_port_t port;
    IOReturn result;
    std::vector<io_user_reference_t> arguments;
};

inline std::vector<SwitchTestSent> switch_test_sent;
inline std::vector<mach_port_t> switch_test_released_ports;
// The kernel's send rights to each port, as MIG and IOUserClient move them: a registration's call brings one, a
// release with none left is the kernel's "send right count overflow" panic.
inline std::map<mach_port_t, int> switch_test_rights;
inline std::function<void(mach_port_t)> switch_test_on_release;

// What the user client hands over as SwitchNVDECCompletionPort (IOUserClient's protected statics).
inline IOReturn switch_test_send(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *arguments,
                                 UInt32 count)
{
    const auto port = mach_port_t(reference[0]);
    EXPECT_GT(switch_test_rights[port], 0) << "a reply on a port the kernel holds no right to";
    switch_test_sent.push_back({port, result, {arguments, arguments + count}});
    return kIOReturnSuccess;
}

inline void switch_test_refer(OSAsyncReference64 reference, mach_port_t port, task_t)
{
    std::memset(reference, 0, sizeof(OSAsyncReference64));
    reference[0] = port;
}

class IOUserClient {
public:
    static IOReturn releaseNotificationPort(mach_port_t port)
    {
        EXPECT_GT(switch_test_rights[port], 0) << "send right count overflow";
        --switch_test_rights[port];
        switch_test_released_ports.push_back(port);
        if (switch_test_on_release)
            switch_test_on_release(port);
        return kIOReturnSuccess;
    }
};

class IOMemoryDescriptor {};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};
