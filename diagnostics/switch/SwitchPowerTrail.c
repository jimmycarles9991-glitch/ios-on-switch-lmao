// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power trail (SwitchPowerTrail.h). The slice: a header line, then a ring of 64-byte lines. A line's seq is
// written last, after the rest and a barrier, and every line is drained with a DSB: a reset between two stores leaves
// at worst one line that fails its CRC, never a torn line that passes.
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include "../../drivers/SwitchSuspend/SwitchSleepSteps.h"
#include "SwitchPanicSave.h"
#include "SwitchPowerTrail.h"
#include "../../kits/SwitchTime/SwitchTime.h"

#if DEVELOPMENT || DEBUG
extern int vsnprintf(char *, size_t, const char *, va_list);
extern uint32_t crc32(uint32_t crc, const void *buf, size_t size);

#define TRAIL_MAGIC UINT64_C(0x3152545048435753) /* "SWCHPTR1" */
#define TEXT_BYTES  48u

struct trail_line {
    uint32_t seq; /* 1-based, the line's place in its boot's trail; 0: empty */
    uint32_t crc; /* of the line with crc 0 */
    uint64_t uptime_ns;
    char text[TEXT_BYTES];
};

struct trail_header {
    uint64_t magic;
    uint32_t boot; /* counts boots that found a trail, so lines from different boots never pass as one */
    uint32_t lines;
    uint8_t pad[sizeof(struct trail_line) - 16];
};

_Static_assert(sizeof(struct trail_line) == 64, "a line is 64 bytes");
_Static_assert(sizeof(struct trail_header) == sizeof(struct trail_line), "the header takes one line");

#define MAX_LINES (16384u / sizeof(struct trail_line) - 1u)

static volatile struct trail_header *header;
static volatile struct trail_line *ring;
static uint32_t count;
static uint32_t next_seq;
static struct trail_line previous[MAX_LINES];
static uint32_t previous_kept, previous_total, previous_boot;
static int previous_found;

static uint32_t line_crc(const struct trail_line *line)
{
    struct trail_line copy = *line;
    copy.crc = 0;
    return crc32(0, &copy, sizeof(copy));
}

static void load_words(void *to, const volatile void *from, size_t bytes)
{
    uint32_t *dst = (uint32_t *)to;
    const volatile uint32_t *src = (const volatile uint32_t *)from;
    for (size_t i = 0; i < bytes / 4; ++i)
        dst[i] = src[i];
}

/* The last boot's lines with a good CRC, oldest first: the highest seq found is the last line written. */
static void keep_previous(void)
{
    struct trail_header old;
    load_words(&old, header, sizeof(old));
    if (old.magic != TRAIL_MAGIC || old.lines != count)
        return;
    previous_found = 1;
    previous_boot = old.boot;
    uint32_t last = 0;
    for (uint32_t i = 0; i < count; ++i) {
        struct trail_line line;
        load_words(&line, &ring[i], sizeof(line));
        if (line.seq && line.crc == line_crc(&line) && (line.seq - 1) % count == i && line.seq > last)
            last = line.seq;
    }
    previous_total = last;
    const uint32_t first = last > count ? last - count + 1 : 1;
    for (uint32_t seq = first; last && seq <= last; ++seq) {
        struct trail_line line;
        load_words(&line, &ring[(seq - 1) % count], sizeof(line));
        if (line.seq == seq && line.crc == line_crc(&line))
            previous[previous_kept++] = line;
    }
}

void switch_power_trail_init(volatile uint8_t *slice, uint32_t bytes)
{
    const uint32_t lines = bytes / sizeof(struct trail_line);
    if (!slice || lines < 2)
        return;
    header = (volatile struct trail_header *)slice;
    ring = (volatile struct trail_line *)(slice + sizeof(struct trail_header));
    count = lines - 1 < MAX_LINES ? lines - 1 : MAX_LINES;
    keep_previous();
    for (uint32_t i = 0; i < count; ++i)
        ring[i].seq = 0;
    header->magic = TRAIL_MAGIC;
    header->boot = previous_boot + 1;
    header->lines = count;
    __asm__ volatile("dsb sy" ::: "memory");
}

void switch_power_trail(const char *format, ...)
{
    char text[160];
    va_list args;  // NOLINT(cppcoreguidelines-init-variables): va_start initializes it
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (ml_get_interrupts_enabled())
        os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: %s", text);
    if (!ring)
        return;
    struct trail_line line = {0};
    for (uint32_t i = 0; i < TEXT_BYTES - 1 && text[i]; ++i)
        line.text[i] = text[i];
    line.uptime_ns = switch_now_ns();
    line.seq = __atomic_add_fetch(&next_seq, 1, __ATOMIC_RELAXED);
    line.crc = line_crc(&line);
    volatile struct trail_line *slot = &ring[(line.seq - 1) % count];
    slot->seq = 0;
    slot->crc = line.crc;
    slot->uptime_ns = line.uptime_ns;
    for (uint32_t i = 0; i < TEXT_BYTES; ++i)
        slot->text[i] = line.text[i];
    __asm__ volatile("dmb oshst" ::: "memory");
    slot->seq = line.seq;
    __asm__ volatile("dsb sy" ::: "memory");
}

void switch_power_trail_watch(unsigned seconds, const char *why)
{
    if (seconds > 255)
        seconds = 255;
    switch_panic_save_watchdog(seconds);
    if (seconds) {
        switch_power_trail("watchdog: warm reset in %u s (%s)", seconds, why);
    } else {
        switch_power_trail("watchdog off (%s)", why);
    }
}

/* SwitchPowerMark.cpp, the MAX77620 RTC's writer; weak, so that a kernel without the PMIC driver keeps marks in the
 * trail only. */
__attribute__((weak)) void switch_pmic_mark(uint8_t code)
{
    (void)code;
}

__attribute__((weak)) void switch_pmic_mark_flush(void)
{}

__attribute__((weak)) void switch_pmic_mark_now(uint8_t code)
{
    (void)code;
}

__attribute__((weak)) void switch_pmic_flag_now(uint8_t flag)
{
    (void)flag;
}

static const char *halt_name(unsigned code)
{
    static const char *const halt[] = {
        "reboot(2) began",
        "IOPMRootDomainWillShutdown and paging off done",
        "proc_shutdown done",
        "another halt step done",
        "first vfs_unmountall done",
        "second proc_shutdown done",
        "second vfs_unmountall done",
        "buffer cache clean",
        "if_down_all done",
        "a driver's systemWillShutdown is slow",
        "power tree quiesced",
        "every driver told",
        "PE_halt_restart",
        "CPU off the DFLL",
        "panel dark, MAX77620 soft reset next",
        "soft reset written, still running 2 s later",
    };
    return halt[code & 0xfu];
}

static const char *mark_name(unsigned code)
{
    switch (code) {
    case SWITCH_MARK_NONE:
        return "running";
    case SWITCH_MARK_BOOT_PMIC:
        return "boot: PMIC up";
    case SWITCH_MARK_BOOT_ROOT:
        return "boot: root mounted";
    case SWITCH_MARK_BOOT_LAUNCHD:
        return "boot: launchd next";
    case SWITCH_MARK_WATCHDOG_BENCH:
        return "watchdog bench: armed, never petted";
    case SWITCH_MARK_BOOT_DONE:
        return "boot complete";
    default:
        break;
    }
    if ((code & 0x70u) == SWITCH_MARK_HALT_BEGAN)
        return halt_name(code);
    if (switch_sleep_step_name(code))
        return switch_sleep_step_name(code);
    switch (code & 0x70u) {
    case SWITCH_MARK_SLEEP:
        return "sleep step";
    case SWITCH_MARK_SC7:
        return "SC7 entry step";
    case SWITCH_MARK_RESUME:
        return "SC7 exit step";
    case SWITCH_MARK_WAKE:
        return code == 0x5fu ? "wake complete" : "wake step";
    default:
        return "unknown";
    }
}

void switch_power_trail_mark(unsigned code)
{
    switch_power_trail("mark %#04x %s", code, mark_name(code & 0x7fu));
    switch_pmic_mark((uint8_t)(code & 0x7fu));
}

void switch_power_trail_mark_flush(void)
{
    switch_pmic_mark_flush();
}

void switch_power_trail_mark_now(unsigned code)
{
    switch_power_trail("mark %#04x %s (now)", code, mark_name(code & 0x7fu));
    switch_pmic_mark_now((uint8_t)(code & 0x7fu));
}

void switch_power_trail_flag_now(unsigned flag)
{
    switch_power_trail("flag %u on the mark (now)", flag);
    switch_pmic_flag_now((uint8_t)flag);
}

static int durable_code = -2; /* -2: not read this boot */
static unsigned durable_count, durable_flags;

static void log_durable(const char *when)
{
    if (durable_code == -2)
        return;
    if (durable_code < 0) {
        os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: durable mark: none (the MAX77620 RTC holds no mark of ours)%s", when);
        return;
    }
    const unsigned code = (unsigned)durable_code;
    os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: durable mark: %#04x %s%s%s (mark %u of the last boot)%s", code,
           mark_name(code),
           (durable_flags & (unsigned)SWITCH_MARK_FLAG_DEADLINE) ? ", then the shutdown deadline passed" : "",
           (durable_flags & (unsigned)SWITCH_MARK_FLAG_PANIC_RESET) ? ", then a panic's warm reset" : "", durable_count,
           when);
}

/* Logged at once and again before launchd: the unified log may not keep what the kernel logs this early. */
void switch_power_trail_durable(int code, unsigned count, unsigned flags)
{
    durable_code = code;
    durable_count = count;
    durable_flags = flags;
    log_durable("");
}

void switch_power_trail_report(void)
{
    static int reported;
    if (reported)
        return;
    reported = 1;
    log_durable(", again");
    switch_power_trail_mark(SWITCH_MARK_BOOT_LAUNCHD);
    if (!ring)
        return;
    if (!previous_found) {
        os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: no trail from the last boot (cold power-on, or DRAM lost)");
        return;
    }
    os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: last boot (%u) wrote %u lines, %u kept", previous_boot, previous_total,
           previous_kept);
    for (uint32_t i = 0; i < previous_kept; ++i) {
        const struct trail_line *line = &previous[i];
        char text[TEXT_BYTES + 1];
        for (uint32_t c = 0; c < TEXT_BYTES; ++c)
            text[c] = line->text[c];
        text[TEXT_BYTES] = 0;
        os_log(OS_LOG_DEFAULT, "SwitchPowerTrail: last boot: #%u %llu.%03llu %s", line->seq,
               (unsigned long long)(line->uptime_ns / 1000000000ULL),
               (unsigned long long)(line->uptime_ns / 1000000ULL % 1000), text);
    }
}
#endif
