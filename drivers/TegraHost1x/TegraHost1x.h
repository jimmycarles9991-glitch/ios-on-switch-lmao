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

// WAIT_SYNCPT (host1x class method 8, Linux hw_host1x05_uclass.h host1x_uclass_wait_syncpt_r): the channel stalls until
// syncpoint `id` reaches `threshold` (low 24 bits), so an engine's stream starts only after another engine's work.
// Writes two words; the stream's next SETCLASS returns it to its engine.
static inline uint32_t tegra_host1x_op_wait_syncpt(uint32_t *w, uint32_t id, uint32_t threshold)
{
    w[0] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_HOST1X, 8, 1);
    w[1] = ((id & 0xff) << 24) | (threshold & 0xffffff);
    return 2;
}

// INCR_SYNCPT (method 0 of every class: the engine's THI implements the host methods) with a condition.
static inline uint32_t tegra_host1x_op_incr_syncpt(uint32_t cond, uint32_t syncpt)
{
    return tegra_host1x_op_imm(0, ((cond & 0xff) << 8) | (syncpt & 0xff));
}

// Brings host1x up once (clock and reset adopted or enabled, sync unit configured, the syncpoint interrupt claimed,
// channel 0's command DMA started) and runs its self-test: a host-class increment completed by interrupt.
// Idempotent; returns TEGRA_HOST1X_OK or the failure, after logging one line either way.
int tegra_host1x_start(void);

// Syncpoints this driver hands out (above the IDs the boot chain and Horizon use: hekate di.c waits on 9).
int tegra_host1x_syncpt_alloc(uint32_t *id);
uint32_t tegra_host1x_syncpt_read(uint32_t id);

// Sleeps until syncpoint `id` reaches `threshold` (wrapping compare) or `timeoutUs` pass. A threshold interrupt that
// is pending at the controller but never reaches the CPU panics (one line, with the GIC's state) once a client has
// proven its engine (tegra_host1x_set_strict); before that it returns TEGRA_HOST1X_ELOST, so a probe that meets a
// missing interrupt logs its own failure line and leaves its engine unpublished instead of stopping the boot.
int tegra_host1x_syncpt_wait(uint32_t id, uint32_t threshold, uint32_t timeoutUs);
void tegra_host1x_set_strict(void);

// Command DMA channels: VIC has its own, so a stream that waits for another engine (WAIT_SYNCPT) holds up only VIC;
// the other engines share the default one.
enum {
    TEGRA_HOST1X_CHANNEL_DEFAULT = 0,
    TEGRA_HOST1X_CHANNEL_VIC = 1,
    TEGRA_HOST1X_CHANNELS = 2
};

// Copies `count` opcode words into a channel's push buffer and starts them. The stream must leave its syncpoint
// increments to the caller's wait; `count` must be below 4096. Serialised; returns at once.
int tegra_host1x_submit_on(uint32_t channel, const uint32_t *words, uint32_t count);
// The same on the default channel.
int tegra_host1x_submit(const uint32_t *words, uint32_t count);

// Sleep (called through tegra_nvdec_suspend/resume below, after NVDEC has quiesced). Suspend saves the
// handed-out syncpoint values and stops channel 0; resume re-adopts the clock, reconfigures the sync unit, restores
// the values and restarts channel 0. Both are no-ops unless host1x is up.
void tegra_host1x_suspend(void);
void tegra_host1x_resume(void);

// The exports the sleep agent (drivers/SwitchSuspend) calls: the registered clients' quiesce then host1x's suspend,
// and host1x's resume then the clients' reload. With nothing registered (NVDEC off) they are host1x alone, which is a
// no-op unless host1x is up. TegraNVDEC, TegraNVENC, TegraVIC and TegraNVJPG register once their engines run (four at
// most; a fifth is refused with a log line and its engine is then not reloaded after SC7). Clients quiesce in the
// reverse order of registration and reload in order, so an engine that registers after NVDEC (NVJPG, whose MBIST
// workaround runs on NVDEC's clock) finds NVDEC already back.
void tegra_host1x_set_sleep_client(void (*suspend)(void), void (*resume)(void));
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
