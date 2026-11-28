// XusbMailbox.h against xhci-tegra.c tegra_xusb_mbox_handle on a T210B01: which messages are answered, with what, and
// that anything else gets no answer and leaves the mailbox to the firmware; and a command's sender, which sleeps until
// the mailbox interrupt brings the firmware's answer.
#include "drivers/SwitchUSB/XusbMailbox.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace {
constexpr uint32_t kFalconKHz = 336000;

TEST(XusbMailbox, FalconRateAckedOnlyWhenItIsTheOneItHas)
{
    for (const uint32_t cmd : {XUSB_MBOX_INC_FALC_CLOCK, XUSB_MBOX_DEC_FALC_CLOCK}) {
        const xusb_mbox_reply same = xusb_mbox_answer(cmd, kFalconKHz, kFalconKHz);
        EXPECT_TRUE(same.reply);
        EXPECT_EQ(same.cmd, XUSB_MBOX_REPLY_ACK);
        EXPECT_EQ(same.data, kFalconKHz);
        const xusb_mbox_reply other = xusb_mbox_answer(cmd, 204000, kFalconKHz);
        EXPECT_TRUE(other.reply);
        EXPECT_EQ(other.cmd, XUSB_MBOX_REPLY_NAK);
        EXPECT_EQ(other.data, kFalconKHz);
    }
}

TEST(XusbMailbox, SspiAndHsicIdleAckedWithTheirData)
{
    for (const uint32_t cmd :
         {XUSB_MBOX_INC_SSPI_CLOCK, XUSB_MBOX_DEC_SSPI_CLOCK, XUSB_MBOX_START_HSIC_IDLE, XUSB_MBOX_STOP_HSIC_IDLE}) {
        const xusb_mbox_reply a = xusb_mbox_answer(cmd, 0x1234, kFalconKHz);
        EXPECT_TRUE(a.reply) << cmd;
        EXPECT_EQ(a.cmd, XUSB_MBOX_REPLY_ACK) << cmd;
        EXPECT_EQ(a.data, 0x1234u) << cmd;
    }
}

// MSG_ENABLED (1), SET_SS_PWR_GATING/UNGATING (7, 8), AIRPLANE_MODE (10, 11), DBC_WAKE_STACK (14),
// HSIC_PRETEND_CONNECT (15) and anything unnumbered: tegra_xusb_mbox_handle's default sends nothing.
TEST(XusbMailbox, OtherMessagesGetNoAnswer)
{
    for (const uint32_t cmd : {1u, 7u, 8u, 10u, 11u, 14u, 15u, 19u, 0x7fu})
        EXPECT_FALSE(xusb_mbox_answer(cmd, 0, kFalconKHz).reply) << cmd;
}

// The sender sleeps on the mailbox's gate the way commandSleep does (the gate released while asleep) and the handler
// takes the gate to deliver the firmware's answer, as mailboxAction runs on the loop whose gate the sender holds. A
// sender that kept the gate while waiting would see its answer only after its deadline, as the 2026-11-14 boot did.
struct Gate {
    std::mutex lock;
    std::condition_variable woken;
    xusb_mbox_wait wait{};
    std::atomic<bool> released{false};  // the firmware let the mailbox go without a message
    unsigned looks = 0;
};

using Clock = std::chrono::steady_clock;

xusb_mbox_outcome sendAndAwait(Gate &gate, uint32_t cmd, std::chrono::milliseconds first,
                               std::chrono::milliseconds deadline, std::atomic<bool> &sent)
{
    std::unique_lock<std::mutex> held(gate.lock);
    xusb_mbox_wait_begin(&gate.wait, cmd);
    sent = true;
    const Clock::time_point looks[2] = {Clock::now() + first, Clock::now() + deadline};
    return xusb::awaitAnswer(
        gate.wait,
        [&](unsigned look) {
            return gate.woken.wait_until(held, looks[look]) != std::cv_status::timeout;
        },
        [&] {
            ++gate.looks;
            return gate.released.load();
        });
}

void answerFromTheInterrupt(Gate &gate, const std::atomic<bool> &sent, uint32_t reply, uint32_t data,
                            std::chrono::milliseconds after = std::chrono::milliseconds(0))
{
    while (!sent)
        std::this_thread::yield();
    std::this_thread::sleep_for(after);
    const std::lock_guard<std::mutex> held(gate.lock);
    if (xusb_mbox_wait_answer(&gate.wait, reply, data))
        gate.woken.notify_all();
}

constexpr auto kFirst = std::chrono::milliseconds(20);
constexpr auto kLong = std::chrono::seconds(10);

// RESET_SSPI on the device: an ACK message, the owner ours until it is handled.
TEST(XusbMailbox, AckArrivingWhileTheSenderSleepsCompletesIt)
{
    Gate gate;
    std::atomic<bool> sent{false};
    std::thread interrupt(answerFromTheInterrupt, std::ref(gate), std::ref(sent), XUSB_MBOX_REPLY_ACK, 0u,
                          std::chrono::milliseconds(0));
    const auto start = Clock::now();
    EXPECT_EQ(sendAndAwait(gate, 16, kLong, kLong, sent), XUSB_MBOX_ACKED);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5)) << "woken by the answer, not the deadline";
    interrupt.join();
    EXPECT_EQ(gate.looks, 0u) << "an answer needs no look at the owner";
    EXPECT_EQ(gate.wait.cmd, 0u) << "the wait is closed";
}

// An answer after the first look still wakes the sender before the deadline.
TEST(XusbMailbox, AckAfterTheFirstLookCompletesIt)
{
    Gate gate;
    std::atomic<bool> sent{false};
    std::thread interrupt(answerFromTheInterrupt, std::ref(gate), std::ref(sent), XUSB_MBOX_REPLY_ACK, 0u, 3 * kFirst);
    const auto start = Clock::now();
    EXPECT_EQ(sendAndAwait(gate, 16, kFirst, kLong, sent), XUSB_MBOX_ACKED);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
    interrupt.join();
    EXPECT_EQ(gate.looks, 1u);
}

TEST(XusbMailbox, NakIsTheCommandRefused)
{
    Gate gate;
    std::atomic<bool> sent{false};
    std::thread interrupt(answerFromTheInterrupt, std::ref(gate), std::ref(sent), XUSB_MBOX_REPLY_NAK, 0x12u,
                          std::chrono::milliseconds(0));
    EXPECT_EQ(sendAndAwait(gate, 1, kLong, kLong, sent), XUSB_MBOX_NAKED);
    interrupt.join();
    EXPECT_EQ(gate.wait.data, 0x12u);
}

// MSG_ENABLED on the device: no message, the mailbox released; taken at the first look.
TEST(XusbMailbox, ReleaseWithoutAMessageIsTakenAtTheFirstLook)
{
    Gate gate;
    gate.released = true;
    std::atomic<bool> sent{false};
    const auto start = Clock::now();
    EXPECT_EQ(sendAndAwait(gate, 1, kFirst, kLong, sent), XUSB_MBOX_RELEASED);
    EXPECT_LT(Clock::now() - start, std::chrono::seconds(5));
    EXPECT_EQ(gate.looks, 1u);
}

// Neither: the owner is looked at twice (the first look and the deadline) and a later answer wakes nobody.
TEST(XusbMailbox, NeitherEndsAtTheDeadlineAfterTwoLooks)
{
    Gate gate;
    std::atomic<bool> sent{false};
    const auto start = Clock::now();
    EXPECT_EQ(sendAndAwait(gate, 16, std::chrono::milliseconds(5), std::chrono::milliseconds(30), sent),
              XUSB_MBOX_WAITING);
    EXPECT_GE(Clock::now() - start, std::chrono::milliseconds(30));
    EXPECT_EQ(gate.looks, 2u);
    EXPECT_FALSE(xusb_mbox_wait_answer(&gate.wait, XUSB_MBOX_REPLY_ACK, 0));
}

// SC7 takes the mailbox down under a waiting command: the sender is woken with no answer.
TEST(XusbMailbox, AbandonedBySleep)
{
    xusb_mbox_wait wait{};
    xusb_mbox_wait_begin(&wait, 1);
    EXPECT_TRUE(xusb_mbox_wait_abandon(&wait));
    EXPECT_FALSE(xusb_mbox_wait_answer(&wait, XUSB_MBOX_REPLY_ACK, 0)) << "the first outcome stands";
    EXPECT_EQ(xusb::awaitAnswer(
                  wait,
                  [](unsigned) {
                      return true;
                  },
                  [] {
                      return true;
                  }),
              XUSB_MBOX_ABANDONED);
    EXPECT_FALSE(xusb_mbox_wait_abandon(&wait)) << "nothing waits any more";
}

// Released: anything but our own ownership (NONE, or the falcon's again for a message of its own).
TEST(XusbMailbox, ReleasedMeansNoLongerOurs)
{
    EXPECT_FALSE(xusb_mbox_released(XUSB_MBOX_OWNER_SW));
    EXPECT_TRUE(xusb_mbox_released(0));
    EXPECT_TRUE(xusb_mbox_released(1));
}

TEST(XusbMailbox, DeferredReplySetTakeCancel)
{
    xusb_mbox_deferred d{};
    xusb_mbox_deferred taken{};
    EXPECT_FALSE(xusb_mbox_deferred_take(&d, &taken));
    EXPECT_FALSE(xusb_mbox_deferred_cancel(&d));

    xusb_mbox_deferred_set(&d, 17u, XUSB_MBOX_REPLY_ACK, 1u << 1, XUSB_MBOX_LFPS_SETTLE_MS);
    EXPECT_TRUE(d.pending);
    EXPECT_EQ(d.delay_ms, 1u);
    EXPECT_TRUE(xusb_mbox_deferred_take(&d, &taken));
    EXPECT_FALSE(d.pending);
    EXPECT_EQ(taken.request_cmd, 17u);
    EXPECT_EQ(taken.reply_cmd, XUSB_MBOX_REPLY_ACK);
    EXPECT_EQ(taken.data, 1u << 1);
    EXPECT_FALSE(xusb_mbox_deferred_take(&d, &taken));

    xusb_mbox_deferred_set(&d, 17u, XUSB_MBOX_REPLY_ACK, 1u << 1, XUSB_MBOX_LFPS_SETTLE_MS);
    EXPECT_TRUE(xusb_mbox_deferred_cancel(&d));
    EXPECT_FALSE(d.pending);
    EXPECT_FALSE(xusb_mbox_deferred_cancel(&d));
    EXPECT_FALSE(xusb_mbox_deferred_take(&d, &taken));
}
}  // namespace
