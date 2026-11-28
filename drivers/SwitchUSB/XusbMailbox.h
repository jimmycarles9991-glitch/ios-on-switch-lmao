// SPDX-License-Identifier: BSD-2-Clause-Patent
// The falcon's mailbox messages other than the SuperSpeed ones (XusbLanes.h xusb_mbox_ss_decide), answered as
// xhci-tegra.c tegra_xusb_mbox_handle answers them on a T210B01 (scale_ss_clock false, no HSIC port). Plain C: the
// host tests include this same header.
#ifndef XUSB_MAILBOX_H
#define XUSB_MAILBOX_H

#include <stdbool.h>
#include <stdint.h>

#define XUSB_MBOX_INC_FALC_CLOCK  2u
#define XUSB_MBOX_DEC_FALC_CLOCK  3u
#define XUSB_MBOX_INC_SSPI_CLOCK  4u
#define XUSB_MBOX_DEC_SSPI_CLOCK  5u
#define XUSB_MBOX_START_HSIC_IDLE 12u
#define XUSB_MBOX_STOP_HSIC_IDLE  13u
#define XUSB_MBOX_REPLY_ACK       128u
#define XUSB_MBOX_REPLY_NAK       129u
#define XUSB_MBOX_OWNER_SW        2u
#define XUSB_MBOX_LFPS_SETTLE_MS  1u

// A command we send is taken one of two ways on 24B119's firmware. RESET_SSPI is answered with an ACK or NAK message,
// which raises the mailbox interrupt, and the owner stays ours until the handler has taken that answer
// (tegra_xusb_mbox_thread clears it for ACK and NAK): "falcon ACK 0, the last command sent 16", the owner still 2.
// MSG_ENABLED gets no message: the firmware releases the mailbox itself within 1 ms, which raises nothing. So the sender
// sleeps on the answer, which the handler gives it, and looks at the owner twice (XusbSettle.h's two looks) for a
// release without one.
enum xusb_mbox_outcome {
    XUSB_MBOX_WAITING,  // neither by the deadline
    XUSB_MBOX_ACKED,
    XUSB_MBOX_NAKED,
    XUSB_MBOX_RELEASED,   // taken without a message: the mailbox no longer ours
    XUSB_MBOX_ABANDONED,  // the mailbox went down (SC7) first
};

// Released: the mailbox no longer ours. The firmware may own it again for a message of its own by the time it is looked
// at, so anything but our own ownership counts.
static inline bool xusb_mbox_released(uint32_t owner)
{
    return owner != XUSB_MBOX_OWNER_SW;
}

struct xusb_mbox_wait {
    uint32_t cmd;  // the command waiting for its answer, 0 none
    enum xusb_mbox_outcome outcome;
    uint32_t data;  // the answer's data
};

static inline void xusb_mbox_wait_begin(struct xusb_mbox_wait *w, uint32_t cmd)
{
    w->cmd = cmd;
    w->outcome = XUSB_MBOX_WAITING;
    w->data = 0;
}

// The handler's ACK or NAK; true when a command was waiting for it (the sender is to be woken).
static inline bool xusb_mbox_wait_answer(struct xusb_mbox_wait *w, uint32_t reply, uint32_t data)
{
    if (!w->cmd || w->outcome != XUSB_MBOX_WAITING)
        return false;
    w->outcome = reply == XUSB_MBOX_REPLY_ACK ? XUSB_MBOX_ACKED : XUSB_MBOX_NAKED;
    w->data = data;
    return true;
}

static inline bool xusb_mbox_wait_abandon(struct xusb_mbox_wait *w)
{
    if (!w->cmd || w->outcome != XUSB_MBOX_WAITING)
        return false;
    w->outcome = XUSB_MBOX_ABANDONED;
    return true;
}

// The sender, done sleeping: its outcome, and the wait closed, so a later answer wakes nobody.
static inline enum xusb_mbox_outcome xusb_mbox_wait_end(struct xusb_mbox_wait *w)
{
    w->cmd = 0;
    return w->outcome;
}

// A falcon mailbox reply deferred behind a one-shot timer (T210 LFPS settle on U3 entry) so the mailbox IRQ handler
// never sleeps on its work loop.
struct xusb_mbox_deferred {
    bool pending;
    uint32_t request_cmd;
    uint32_t reply_cmd;
    uint32_t data;
    uint32_t delay_ms;
};

static inline void xusb_mbox_deferred_set(struct xusb_mbox_deferred *d, uint32_t request_cmd, uint32_t reply_cmd,
                                          uint32_t data, uint32_t delay_ms)
{
    d->pending = true;
    d->request_cmd = request_cmd;
    d->reply_cmd = reply_cmd;
    d->data = data;
    d->delay_ms = delay_ms;
}

static inline bool xusb_mbox_deferred_take(struct xusb_mbox_deferred *d, struct xusb_mbox_deferred *out)
{
    if (!d->pending)
        return false;
    *out = *d;
    d->pending = false;
    return true;
}

static inline bool xusb_mbox_deferred_cancel(struct xusb_mbox_deferred *d)
{
    if (!d->pending)
        return false;
    d->pending = false;
    return true;
}

struct xusb_mbox_reply {
    bool reply;  // false: no answer, and the mailbox stays the firmware's (tegra_xusb_mbox_thread clears the owner only
                 // for SET_BW, ACK and NAK)
    uint32_t cmd;
    uint32_t data;
};

// The falcon's own rate is ACKed and any other NAKed, with the rate it has; SSPI scaling and HSIC idle are ACKed with
// their data.
static inline struct xusb_mbox_reply xusb_mbox_answer(uint32_t cmd, uint32_t data, uint32_t falcon_khz)
{
    struct xusb_mbox_reply a = {true, XUSB_MBOX_REPLY_ACK, data};
    switch (cmd) {
    case XUSB_MBOX_INC_FALC_CLOCK:
    case XUSB_MBOX_DEC_FALC_CLOCK:
        a.data = falcon_khz;
        a.cmd = data == falcon_khz ? XUSB_MBOX_REPLY_ACK : XUSB_MBOX_REPLY_NAK;
        break;
    case XUSB_MBOX_INC_SSPI_CLOCK:
    case XUSB_MBOX_DEC_SSPI_CLOCK:
    case XUSB_MBOX_START_HSIC_IDLE:
    case XUSB_MBOX_STOP_HSIC_IDLE:
        break;
    default:
        a.reply = false;
        break;
    }
    return a;
}

#ifdef __cplusplus
namespace xusb {
// The sender's wait, on whatever releases the mailbox handler's lock while it sleeps: the mailbox work loop's
// IOCommandGate::commandSleep in the kernel, a condition variable in the tests. sleepUntil(look) sleeps until woken or
// that look's time (0 the first, 1 the deadline) and returns false once it has passed; released() reads the owner.
template <typename Sleep, typename Released>
enum xusb_mbox_outcome awaitAnswer(struct xusb_mbox_wait &wait, Sleep sleepUntil, Released released)
{
    for (unsigned look = 0; look < 2 && wait.outcome == XUSB_MBOX_WAITING; ++look) {
        while (wait.outcome == XUSB_MBOX_WAITING && sleepUntil(look)) {
        }
        if (wait.outcome == XUSB_MBOX_WAITING && released())
            wait.outcome = XUSB_MBOX_RELEASED;
    }
    return xusb_mbox_wait_end(&wait);
}
}  // namespace xusb
#endif

#endif
