// SPDX-License-Identifier: BSD-2-Clause-Patent
// Tegra210 host1x (version 5) for the engines behind it: syncpoints with threshold interrupts, and command DMA
// channels that feed an engine's class (NVDEC, VIC, NVJPG, NVENC) a stream of host1x opcodes.
//
// Kernel API for host1x clients (drivers/TegraNVDEC). Everything is thread context; waits sleep until the syncpoint
// threshold interrupt (GIC SPI 65) and never poll. host1x is the bus every engine at 0x54000000 sits behind, and the
// boot chain leaves it clocked for the display (hekate clock_enable_host1x, di.c), so it is adopted, never reset.
#ifndef TEGRA_HOST1X_H
#define TEGRA_HOST1X_H
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    TEGRA_HOST1X_OK = 0,
    TEGRA_HOST1X_ENODEV = -19,  // not started (no client asked: switch-no-nvdec) or failed
    TEGRA_HOST1X_EINVAL = -22,
    TEGRA_HOST1X_ENOMEM = -12,
    TEGRA_HOST1X_ETIMEDOUT = -60,  // the engine did not reach the threshold in time (its interrupt was not lost)
    TEGRA_HOST1X_ELOST = -5,       // the threshold was reached but its interrupt never came (before strict mode only)
    TEGRA_HOST1X_EAGAIN = -35,     // no command arena room until earlier jobs' fences pass
};

// Host1x class IDs (Linux include/linux/host1x.h: HOST1X_CLASS_HOST1X, ..._NVENC, ..._VIC, ..._NVJPG, ..._NVDEC).
enum {
    TEGRA_HOST1X_CLASS_HOST1X = 0x01,
    TEGRA_HOST1X_CLASS_NVENC = 0x21,
    TEGRA_HOST1X_CLASS_VIC = 0x5d,
    TEGRA_HOST1X_CLASS_NVJPG = 0xc0,
    TEGRA_HOST1X_CLASS_NVDEC = 0xf0,
};

// Syncpoint increment conditions (host1x uclass INCR_SYNCPT bits 15:8).
enum {
    TEGRA_HOST1X_COND_IMMEDIATE = 0,
    TEGRA_HOST1X_COND_OP_DONE = 1
};

// Opcodes (Linux drivers/gpu/host1x/hw/opcodes.h). Method offsets are in words.
static inline uint32_t tegra_host1x_op_setclass(uint32_t cls, uint32_t offset, uint32_t mask)
{
    return (offset << 16) | (cls << 6) | mask;
}

static inline uint32_t tegra_host1x_op_incr(uint32_t offset, uint32_t count)
{
    return (1u << 28) | (offset << 16) | count;
}

static inline uint32_t tegra_host1x_op_nonincr(uint32_t offset, uint32_t count)
{
    return (2u << 28) | (offset << 16) | count;
}

static inline uint32_t tegra_host1x_op_imm(uint32_t offset, uint32_t value)
{
    return (4u << 28) | (offset << 16) | (value & 0xffff);
}

// T210 host1x05 methods 0x4e/0x50 load the full threshold and wait on it; the legacy method 8 truncates to 24 bits.
enum {
    SWITCH_HOST1X_WAIT_WORDS = 3
};

static inline uint32_t tegra_host1x_op_wait_syncpt(uint32_t *w, uint32_t id, uint32_t threshold)
{
    w[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 0x4e, 5);
    w[1] = threshold;
    w[2] = id;
    return SWITCH_HOST1X_WAIT_WORDS;
}

// INCR_SYNCPT (method 0 of every class: the engine's THI implements the host methods) with a condition.
static inline uint32_t tegra_host1x_op_incr_syncpt(uint32_t cond, uint32_t syncpt)
{
    return tegra_host1x_op_imm(0, ((cond & 0xff) << 8) | (syncpt & 0xff));
}

// Brings host1x up once (clock and reset adopted or enabled, sync unit configured, the syncpoint interrupt claimed,
// channel 0's command DMA started); each engine's own proof of life is the first increment completed by interrupt.
// Idempotent; returns TEGRA_HOST1X_OK or the failure, after logging one line either way.
int tegra_host1x_start(void);

// Syncpoints this driver hands out (above the IDs the boot chain and Horizon use: hekate di.c waits on 9).
int tegra_host1x_syncpt_alloc(uint32_t *id);
uint32_t tegra_host1x_syncpt_read(uint32_t id);
// Only after the exclusive producer stopped and retained completion records say failed; this never publishes success.
int tegra_host1x_syncpt_release_failed(uint32_t id, uint32_t threshold, uint32_t max_increments);

// A stopped engine's cancelled jobs' increments, done by the CPU up to `threshold` (two at a time, the most
// tegra_host1x_syncpt_release_failed takes): another engine's stream waiting on one of them goes on, and the engine's
// queue renumbers from the syncpoint.
static inline int tegra_host1x_syncpt_release_through(uint32_t id, uint32_t threshold)
{
    for (uint32_t now = tegra_host1x_syncpt_read(id); (int32_t)(threshold - now) > 0;
         now = tegra_host1x_syncpt_read(id)) {
        const uint32_t step = threshold - now > 2 ? 2 : threshold - now;
        const int error = tegra_host1x_syncpt_release_failed(id, now + step, step);
        if (error)
            return error;
    }
    return 0;
}

// Sleeps until syncpoint `id` reaches `threshold` (wrapping compare) or `timeoutUs` pass, on a fence of the
// syncpoint's list (SwitchHost1xFence.h), so any number of waiters and fences share a syncpoint. A threshold the
// syncpoint reached whose interrupt never reaches the CPU panics (one line, with the GIC's state) once a client has
// proven its engine (tegra_host1x_set_strict); before that it returns TEGRA_HOST1X_ELOST, so a probe that meets a
// missing interrupt logs its own failure line and leaves its engine unpublished instead of stopping the boot.
int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs);
void tegra_host1x_set_strict(void);

// Command DMA channels, one per engine as mainline gives every client its own (vic.c, nvdec.c, nvjpg.c
// host1x_channel_request; T210 has 14): a stream that waits for another engine (WAIT_SYNCPT) holds up only its own
// engine. NVDEC keeps channel 0.
enum {
    TEGRA_HOST1X_CHANNEL_DEFAULT = 0,
    TEGRA_HOST1X_CHANNEL_VIC = 1,
    TEGRA_HOST1X_CHANNEL_NVENC = 2,
    TEGRA_HOST1X_CHANNEL_NVJPG = 3,
    TEGRA_HOST1X_CHANNELS = 4
};

// A job's stream is written in place in its channel's command arena (below 4 GiB) and host1x gathers it from there, so
// nothing copies it. Reserve room for up to `capacity` words, write the stream (it starts with a SETCLASS), then submit
// the words [first, first + count) of the reservation with the job's own fence: its room is reused once `syncpt`
// reaches `threshold`. Submit and cancel end the reservation; a channel has one open at a time.
// Reserve never sleeps: TEGRA_HOST1X_EAGAIN means the room is held by jobs in flight or another reservation, and
// wait_room sleeps until a reservation would succeed. A caller holding a lock that a fence callback takes (the job
// queues retire from host1x's work loop) drops it before wait_room; begin is reserve and wait_room in a loop.
enum {
    SWITCH_HOST1X_ARENA_WORDS = 8192
};

int switch_host1x_job_reserve(uint32_t channel, uint32_t capacity, uint32_t **words);
int switch_host1x_job_wait_room(uint32_t channel, uint32_t capacity);
int switch_host1x_job_begin(uint32_t channel, uint32_t capacity, uint32_t **words);
int switch_host1x_job_submit(uint32_t channel, const uint32_t *first, uint32_t count, uint32_t syncpt,
                             uint32_t threshold);
void switch_host1x_job_cancel(uint32_t channel);

// Copies `count` opcode words into the channel's arena and gathers them with their fence, for one-off streams (an
// engine's proof of life). Never sleeps: TEGRA_HOST1X_ENOMEM when there is no room.
int switch_host1x_job_copy(uint32_t channel, const uint32_t *words, uint32_t count, uint32_t syncpt,
                           uint32_t threshold);
// The same without a fence: its room comes back with the channel's next fenced job, so a channel holds at most 63 of
// these in a row.
int tegra_host1x_submit_on(uint32_t channel, const uint32_t *words, uint32_t count);
// The same on the default channel.
int tegra_host1x_submit(const uint32_t *words, uint32_t count);
// A job its engine stopped making progress on: teardown drops the channel's pending methods (its engine is reset next),
// restart resumes it empty at its PUT. The caller holds off its own submits in between.
void tegra_host1x_channel_teardown(uint32_t channel);
void tegra_host1x_channel_restart(uint32_t channel);

// Sleep (called through tegra_nvdec_suspend/resume below, after NVDEC has quiesced). Suspend saves the
// handed-out syncpoint values and stops every channel; resume re-adopts the clock, reconfigures the sync unit, restores
// the values and restarts the channels. Both are no-ops unless host1x is up.
void tegra_host1x_suspend(void);
void tegra_host1x_resume(void);

// The exports the sleep agent (drivers/SwitchSuspend) calls: the registered clients' quiesce then host1x's suspend,
// and host1x's resume then the clients' reload. With nothing registered (NVDEC off) they are host1x alone, which is a
// no-op unless host1x is up. TegraNVDEC, TegraNVENC, TegraVIC and TegraNVJPG register once their engines run (four at
// most; a fifth is refused with a log line and its engine is then not reloaded after SC7). Clients quiesce in the
// reverse order of registration and reload in order, so an engine that registers after NVDEC (NVJPG, whose MBIST
// workaround runs on NVDEC's clock) finds NVDEC already back.
void tegra_host1x_set_sleep_client(void (*suspend)(), void (*resume)());
void tegra_nvdec_suspend(void);
void tegra_nvdec_resume(void);

// A channel's state for a failure line: DMAGET, DMAPUT, CBSTAT (class and offset), CBREAD (last word).
void tegra_host1x_describe_channel(uint32_t channel, char *buffer, size_t size);
// The default channel's.
void tegra_host1x_describe(char *buffer, size_t size);

#ifdef __cplusplus
}
#endif
#endif
