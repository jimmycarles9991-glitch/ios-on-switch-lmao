// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVENCListener.c over a real Mach port (sendAsyncResult64 4-argument layout) and refused-port single-Wait fallback.
#include <IOKit/IOKitLib.h>
#include <IOKit/OSMessageNotification.h>
#include <dispatch/dispatch.h>
#include <gtest/gtest.h>
#include <mach/mach.h>
#include <cstring>
#include <vector>
extern "C" {
#include "drivers/SwitchVideoEncoders/AVE/NVENCListener.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-const-variable"
#pragma clang diagnostic ignored "-Wunused-variable"
#include "drivers/TegraNVENC/nvidia/nvenc_drv.h"
#pragma clang diagnostic pop
}

namespace {
mach_port_t gRegistered = MACH_PORT_NULL;
kern_return_t gRegisterResult = KERN_SUCCESS;
int gWaitCalls = 0;
uint32_t gLastWaitFence = 0;
kern_return_t gWaitResult = KERN_SUCCESS;

struct Heard {
    uint32_t fence;
    int result;
    uint32_t errorStatus, ucodeErrorStatus, totalBitCount;
};

std::vector<Heard> gHeard;
dispatch_semaphore_t gAnswered;

void done(void *, uint32_t fence, int result, uint32_t errorStatus, uint32_t ucodeErrorStatus, uint32_t totalBitCount)
{
    gHeard.push_back({fence, result, errorStatus, ucodeErrorStatus, totalBitCount});
    dispatch_semaphore_signal(gAnswered);
}

struct __attribute__((packed)) Completion {
    mach_msg_header_t header;
    mach_msg_size_t size;
    natural_t type;
    io_user_reference_t reference[kOSAsyncRef64Count];
    IOReturn result;
    io_user_reference_t arguments[SWITCH_NVENC_COMPLETION_ARGS];
};

void send(IOReturn result, uint32_t fence, uint32_t errorStatus, uint32_t ucodeErrorStatus, uint32_t totalBitCount)
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
    m.arguments[1] = errorStatus;
    m.arguments[2] = ucodeErrorStatus;
    m.arguments[3] = totalBitCount;

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
    EXPECT_EQ(type, uint32_t(SWITCH_NVENC_COMPLETION_PORT));
    if (gRegisterResult == KERN_SUCCESS)
        gRegistered = port;
    return gRegisterResult;
}

extern "C" kern_return_t nvenc_tunnel(io_connect_t, uint32_t, uint32_t method, const void *in, size_t inSize, void *,
                                      size_t)
{
    EXPECT_EQ(method, uint32_t(TegraNVENCMethodWait));
    EXPECT_EQ(inSize, sizeof(TegraNVENCWait));
    const auto *w = static_cast<const TegraNVENCWait *>(in);
    EXPECT_EQ(w->invalidate_count, 1u);
    gWaitCalls++;
    gLastWaitFence = w->fence;
    return gWaitResult;
}

TEST(NVENCListener, PortRouteDeliversStatusAndBitCountWithZeroWaits)
{
    gRegistered = MACH_PORT_NULL;
    gRegisterResult = KERN_SUCCESS;
    gWaitCalls = 0;
    gHeard.clear();

    dispatch_queue_t queue = dispatch_queue_create("com.brunostjohn.switch.ave.listener-test", DISPATCH_QUEUE_SERIAL);
    gAnswered = dispatch_semaphore_create(0);
    NVENCListener *listener = nvenc_listener_start(1, 0, queue, done, nullptr);
    ASSERT_NE(listener, nullptr);
    ASSERT_TRUE(nvenc_listener_on_port(listener));

    const TegraNVENCRange range = {3, 0, NVENC_STATUS_BYTES, 0};
    nvenc_listener_submitted(listener, 41, range, nullptr);
    nvenc_listener_submitted(listener, 42, range, nullptr);

    send(kIOReturnIOError, 41, 0, 0, 0);
    send(kIOReturnSuccess, 42, 2, 0, 4096);
    for (int i = 0; i < 2; ++i)
        ASSERT_EQ(dispatch_semaphore_wait(gAnswered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    nvenc_listener_stop(listener);

    EXPECT_EQ(gWaitCalls, 0);
    ASSERT_EQ(gHeard.size(), 2u);
    EXPECT_EQ(gHeard[0].fence, 41u);
    EXPECT_EQ(gHeard[0].result, -5);
    EXPECT_EQ(gHeard[1].fence, 42u);
    EXPECT_EQ(gHeard[1].result, 0);
    EXPECT_EQ(gHeard[1].errorStatus, 2u);
    EXPECT_EQ(gHeard[1].ucodeErrorStatus, 0u);
    EXPECT_EQ(gHeard[1].totalBitCount, 4096u);
    dispatch_release(gAnswered);
    dispatch_release(queue);
}

TEST(NVENCListener, RefusedPortFallsBackToSingleWaitPerJob)
{
    gRegistered = MACH_PORT_NULL;
    gRegisterResult = kIOReturnNotPermitted;
    gWaitCalls = 0;
    gWaitResult = KERN_SUCCESS;
    gHeard.clear();

    dispatch_queue_t queue =
        dispatch_queue_create("com.brunostjohn.switch.ave.listener-fallback-test", DISPATCH_QUEUE_SERIAL);
    gAnswered = dispatch_semaphore_create(0);
    NVENCListener *listener = nvenc_listener_start(1, 0, queue, done, nullptr);
    ASSERT_NE(listener, nullptr);
    ASSERT_FALSE(nvenc_listener_on_port(listener));

    nvenc_pic_stat_s stat = {};
    stat.error_status = 2;
    stat.total_bit_count = 1024;
    const TegraNVENCRange range = {7, 0, NVENC_STATUS_BYTES, 0};
    nvenc_listener_submitted(listener, 55, range, &stat);
    ASSERT_EQ(dispatch_semaphore_wait(gAnswered, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC)), 0);
    nvenc_listener_stop(listener);

    EXPECT_EQ(gWaitCalls, 1);
    EXPECT_EQ(gLastWaitFence, 55u);
    ASSERT_EQ(gHeard.size(), 1u);
    EXPECT_EQ(gHeard[0].fence, 55u);
    EXPECT_EQ(gHeard[0].result, 0);
    EXPECT_EQ(gHeard[0].errorStatus, 2u);
    EXPECT_EQ(gHeard[0].totalBitCount, 1024u);
    dispatch_release(gAnswered);
    dispatch_release(queue);
}
