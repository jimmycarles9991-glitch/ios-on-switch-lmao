// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDECListener.c over a real Mach port: the kernel's completion messages (IOUserClient::sendAsyncResult64, as
// SwitchNVDECCompletions.cpp sends them) answered on the session's queue, a failed job's with a failure.
#include <IOKit/IOKitLib.h>
#include <IOKit/OSMessageNotification.h>
#include <dispatch/dispatch.h>
#include <gtest/gtest.h>
#include <mach/mach.h>
#include <cstring>
#include <vector>
extern "C" {
#include "drivers/SwitchVideoDecoders/AVD/NVDECListener.h"
}

namespace {
mach_port_t gRegistered = MACH_PORT_NULL;

struct Heard {
    uint32_t fence;
    int result;
    uint32_t error, correct;
};

std::vector<Heard> gHeard;
dispatch_semaphore_t gAnswered;

void done(void *, uint32_t fence, int result, uint32_t error, uint32_t correct)
{
    gHeard.push_back({fence, result, error, correct});
    dispatch_semaphore_signal(gAnswered);
}

struct __attribute__((packed)) Completion {
    mach_msg_header_t header;
    mach_msg_size_t size;
    natural_t type;
    io_user_reference_t reference[kOSAsyncRef64Count];
    IOReturn result;
    io_user_reference_t arguments[SWITCH_NVDEC_COMPLETION_ARGS];
};

void send(IOReturn result, uint32_t fence, uint32_t error)
{
    Completion m = {};
    m.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    m.header.msgh_size = sizeof(m);
    m.header.msgh_remote_port = gRegistered;
    m.header.msgh_id = kOSNotificationMessageID;
    m.size = sizeof(IOReturn) + sizeof(m.arguments);
    m.type = kIOAsyncCompletionNotificationType;
    m.result = result;
    m.arguments[0] = fence;
    m.arguments[1] = error;

    union {
        mach_msg_header_t header;
        uint8_t bytes[sizeof(Completion)];
    } aligned;

    memcpy(aligned.bytes, &m, sizeof(m));
    EXPECT_EQ(
        mach_msg(&aligned.header, MACH_SEND_MSG, sizeof(m), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL),
        MACH_MSG_SUCCESS);
}
}  // namespace

extern "C" kern_return_t IOConnectSetNotificationPort(io_connect_t, uint32_t type, mach_port_t port, uintptr_t)
{
    EXPECT_EQ(type, uint32_t(SWITCH_NVDEC_COMPLETION_PORT));
    gRegistered = port;
    return KERN_SUCCESS;
}

extern "C" kern_return_t nvdec_tunnel(io_connect_t, uint32_t, uint32_t, const void *, size_t, void *, size_t)
{
    ADD_FAILURE() << "the port route makes no Wait";
    return KERN_FAILURE;
}

// A decode the kernel cancelled (its completion sent with kIOReturnIOError) is answered with a failure, which the AVD
// plug-in's emitter turns into the frame's error; the job after it is answered as decoded.
TEST(NVDECListener, AFailedJobsCompletionAnswersItsPictureWithAFailure)
{
    dispatch_queue_t queue = dispatch_queue_create("com.brunostjohn.switch.avd.listener-test", DISPATCH_QUEUE_SERIAL);
    gAnswered = dispatch_semaphore_create(0);
    NVDECListener *listener = nvdec_listener_start(1, 0, queue, done, nullptr);
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(nvdec_listener_on_port(listener));
    send(kIOReturnIOError, 41, 0);
    send(kIOReturnSuccess, 42, 0x2c);
    for (int i = 0; i < 2; ++i)
        ASSERT_EQ(dispatch_semaphore_wait(gAnswered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0)
            << "a completion was never answered";
    nvdec_listener_stop(listener);
    ASSERT_EQ(gHeard.size(), 2u);
    EXPECT_EQ(gHeard[0].fence, 41u);
    EXPECT_EQ(gHeard[0].result, -5);
    EXPECT_EQ(gHeard[1].fence, 42u);
    EXPECT_EQ(gHeard[1].result, 0);
    EXPECT_EQ(gHeard[1].error, 0x2cu);
    dispatch_release(gAnswered);
    dispatch_release(queue);
}
