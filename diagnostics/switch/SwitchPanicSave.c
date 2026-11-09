// SPDX-License-Identifier: BSD-2-Clause-Patent
// The panic screen and the persistent panic record.
//
// Screen (native profile: in graphics mode always, in text mode once the GUI took it): XNU's console was released at
// the GUI takeover (kPEReleaseScreen frees its text buffers). Taking it back at panic time (SwitchDiag's
// kPEAcquireScreen) has gc_enable allocate them again, kalloc_data(Z_WAITOK) with interrupts off, which this kernel
// refuses: MACH_ASSERT is on, and zalloc_ext asserts "Calling {k,z}alloc from interrupt disabled context isn't allowed"
// (kernel_debugger_entry_count is still 0 there). That nested panic leaves disableConsoleOutput set, so not one line
// reached the screen NativeLegacyDisplay's panic hook had just blacked, and XNU then spins in SavePanicInfo. Here the
// panic is drawn straight into the boot framebuffer, which that hook gives window A back as hekate's pitch window: no
// allocation, no lock, ISO 8x16 glyphs at 1280x720 (XNU's console font and layout). It is drawn at panic entry, again
// once the other CPUs are halted (a frame copy on another CPU may still have landed over it), and once XNU wrote its
// panic log.
//
// Record: hekate's L4T pstore RAM, 0xb0000000 + 2 MiB (hekate bdk/memory_map.h
// PSTORE_ADDR/PSTORE_SZ). The loader's RAM ledger already keeps it from the VM (scripts/build_native_loader.py exclusion
// 'ramoops', the live FDT's ramoops carve-out); _init checks that every page is reserved and saves nothing otherwise.
//   +0x000000  1 MiB    XNU's panic buffer for this boot (gPanicBase: embedded_panic_header, panic log, stackshot). Without
//                       one (no /pram node here) SavePanicInfo spins at its first line and no backtrace is ever written.
//   +0x100000  record   magic, CRC32s (zlib's, also per 4 KiB block), time, phase
//   +0x101000  header   the panic stackshot kept with the record: magic, length, CRC32 (struct switch_panic_stackshot)
//   +0x102000  488 KiB  that stackshot: XNU's (zlib-compressed kcdata, every task's threads with kernel and user frames
//                       and wait info), copied out of XNU's buffer once its panic log is written, as the next boot's XNU
//                       reuses that buffer. Written next to the text as panic-<UTC>.kcdata (pull_logs decodes it)
//   +0x17c000  16 KiB   the power trail (SwitchPowerTrail.h), debug kernels
//   +0x180000  512 KiB  the text, in hekate's L4T console layout: 'DBGC', start, size, then size bytes (main.c _show_errors)
// The text is the panic string, the last 64 KiB of the kernel log (msgbuf) and, once SavePanicInfo returned, XNU's panic
// log (backtraces, kexts). With hekate_on, PMC_SCRATCH37 would be set at panic entry to hekate's L4T panic magic
// 'TPAN' (off: hekate would add SD:/L4T_panic.* files and wait for a button). TMR5/WDT0 is armed to reset through the PMC (Linux tegra_wdt.c: 1 MHz timer at 250 ms, reset on the 4th WDT
// expiry) in case the rest of the panic path hangs. After the panic log, reboot_s seconds after the panic
// (0: never, the panic screen stays up for a photo) the PMC main reset (PMC_CNTRL bit 4, L4T pmc.c tegra_pmc_restart_notify) warm-resets the SoC:
// the PMIC keeps DRAM powered, hekate comes back with RAM as it was, saves the text to SD:/L4T_panic.txt (and the 2 MiB to
// SD:/L4T_panic.bin) and waits for a button. The next iPadOS boot writes /private/var/Switch/panic-<UTC>.log on Data
// (kern_exec.c, before launchd) and clears the record. A record whose panic log was never appended (reset by the
// watchdog) gets XNU's log from +0x000000 at the next boot, before XNU clears that buffer.
// Holding POWER instead (a PMIC power-off) loses DRAM and the record with it.
// A watchdog panic (the userspace watchdog's "userspace watchdog timeout: ...", or the kernel's "watchdog timeout: no
// checkins from watchdogd ...") is a hang, not a bug report to photograph: it warm-resets
// 60 s after the panic (WATCHDOG_REBOOT_S), so the device restarts and the record (with the hung process's threads in the
// stackshot) reaches Data, where holding POWER would have lost it.
// A panic with the panel off (display power: the Switch was locked) has no screen to read or photograph either: it
// warm-resets DARK_REBOOT_S after the panic, for the same reason (r7t panicked locked, looked
// like a Switch that would not wake, and was forced off, record and all). A panic once the kernel is shutting down
// (get_system_inshutdown: reboot(2) has begun) warm-resets SHUTDOWN_REBOOT_S after it: the panel is dark or about to
// be, the console was meant to go off, and POWER held would lose the record within seconds.
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <mach/boolean.h>
#include <mach/vm_types.h>
#include <kern/misc_protos.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/debug.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include <pexpert/pexpert.h>
#include <sys/msgbuf.h>
#include "SwitchDiag.h"
#include "SwitchResetCause.h"
#include "SwitchDiagDisplay.h"
#include "SwitchPanicSave.h"
#include "SwitchPowerTrail.h"

/* SwitchBrightness (SwitchBacklight.cpp): false from a display-off request until the panel is lit again; a plain read.
 * This weak default (a kernel without the backlight) has the panel lit. */
/* NOLINTNEXTLINE(misc-use-internal-linkage): the weak default SwitchBrightness overrides */
__attribute__((weak)) _Bool switch_display_is_on(void)
{
    return true;
}

extern int vsnprintf(char *, size_t, const char *, va_list);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern int snprintf(char *, size_t, const char *, ...);

/* osfmk/kern/clock.c: lock-free (a panic may hold the clock lock) and in RELEASE, which has no DTrace. */
extern void switch_clock_calendar_nowait(clock_sec_t *secs, clock_nsec_t *nanosecs);
extern uint32_t crc32(uint32_t crc, const void *buf, size_t size);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern uint64_t mach_absolute_time(void);

extern boolean_t switch_ram_reserved(ppnum_t pnum);
extern int get_system_inshutdown(void);
extern unsigned char iso_font[256 * 16];

#define REGION_PA         UINT64_C(0xb0000000)
#define REGION_BYTES      0x200000u
#define XNU_LOG_BYTES     0x100000u
#define RECORD_OFFSET     0x100000u
#define CONSOLE_OFFSET    0x180000u
#define CONSOLE_BYTES     0x80000u
#define CONSOLE_SIG       0x43474244u          /* 'DBGC' (hekate PSTORE_RAM_SIG) */
#define TEXT_MAX          (CONSOLE_BYTES - 16) /* hekate saves it only below 0x80000 */
#define BLOCK_BYTES       4096u
#define RECORD_MAGIC      UINT64_C(0x31434e5048435753) /* "SWCHPNC1" */
#define SHOT_HEADER       (RECORD_OFFSET + 0x1000u)
#define SHOT_OFFSET       (RECORD_OFFSET + 0x2000u)
#define TRAIL_BYTES       0x4000u
#define TRAIL_OFFSET      (CONSOLE_OFFSET - TRAIL_BYTES)
#define SHOT_MAX          (TRAIL_OFFSET - SHOT_OFFSET)
#define SHOT_MAGIC        UINT64_C(0x314b534848435753) /* "SWCHHSK1" */
#define WATCHDOG_REBOOT_S 60u
#define DARK_REBOOT_S     15u
#define SHUTDOWN_REBOOT_S 3u
#define EARLY_REBOOT_S    15u
#define MSGBUF_TAIL       (64u << 10)
#define PMC_PA            UINT64_C(0x7000e000) /* the page; APBDEV_PMC at +0x400 (hekate t210.h PMC_BASE) */
#define PMC_CNTRL         (0x400u + 0x000u)
#define PMC_SCRATCH0      (0x400u + 0x050u)
#define PMC_SCRATCH37     (0x400u + 0x130u)
#define PMC_RST_STATUS    (0x400u + 0x1b4u)
#define PMC_MAIN_RST      (1u << 4)
#define PMC_SCRATCH0_MODES \
    ((1u << 0) | (1u << 1) | (1u << 29) | (1u << 30) | (1u << 31)) /* warmboot, RCM, payload, bootloader, recovery */
#define TPAN        0x4e415054u                                    /* hekate PMC_SCRATCH37_KERNEL_PANIC_MAGIC */
#define TMR_PA      UINT64_C(0x60005000)
#define TMR5_PTV    0x060u
#define WDT0_CFG    0x100u
#define WDT0_CMD    0x108u
#define WDT0_UNLOCK 0x10cu

struct switch_panic_record {
    uint64_t magic;
    uint32_t version, bytes;
    uint32_t text_bytes, text_crc;
    uint32_t phase, cpu;                /* phase 1: panic string and kernel log; 2: XNU's panic log appended */
    uint64_t caller, uptime_ns, time_s; /* time_s: the wall clock (UTC) at the panic */
    uint32_t time_ns, blocks;
    uint32_t block_crc[CONSOLE_BYTES / BLOCK_BYTES];
    uint32_t header_crc; /* of everything above */
};

struct switch_panic_console {
    uint32_t sig, start, size;
    char text[];
};

struct switch_panic_stackshot {
    uint64_t magic;
    uint32_t bytes, crc;  /* the stackshot at SHOT_OFFSET and its CRC32 */
    uint64_t panic_flags; /* the embedded panic header's eph_panic_flags (STACKSHOT_DATA_COMPRESSED, ...) */
    uint32_t header_crc;  /* of everything above */
};

static unsigned save_on, hekate_on, reboot_s; /* 0: the panic screen stays up (10-31: the user photographs it) */
static const unsigned reboot_arg = 0;         /* a fixed reboot delay: it would decide for watchdog panics too */
static volatile uint8_t *region;              /* write-combined */
static volatile uint32_t *pmc, *tmr, *screen;
static const char *save_off_reason = "not armed";
static uint32_t last_reset = 0xffffffffu, last_reset_status;
static int pending, pending_damaged;
/* The last boot's end when no panic tells it (SwitchResetCause.h): filed as reset-<UTC>.log by kern_exec.c. */
static int thermal_sensor, thermal_millic, report_pending;
static char report_text[512];
static volatile uint32_t begun;
static int screen_taken;
static unsigned panic_cpu;
static uint64_t panic_at;
static char panic_line[1024];

static volatile struct switch_panic_record *record(void)
{
    return (volatile struct switch_panic_record *)(region + RECORD_OFFSET);
}

static volatile struct switch_panic_console *console(void)
{
    return (volatile struct switch_panic_console *)(region + CONSOLE_OFFSET);
}

static volatile struct switch_panic_stackshot *shot(void)
{
    return (volatile struct switch_panic_stackshot *)(region + SHOT_HEADER);
}

static void drain(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t crc_of(const volatile void *bytes, size_t length)
{
    return crc32(0, (const void *)(uintptr_t)bytes, length);
}

/* --- the record --------------------------------------------------------------------------------------------- */

static uint32_t text_at; /* bytes of console()->text written so far */

static void put(const char *bytes, size_t length)
{
    volatile char *text = console()->text;
    for (size_t i = 0; i < length && text_at < TEXT_MAX; ++i)
        text[text_at++] = bytes[i];
}

static void puts_(const char *s)
{
    size_t n = 0;
    while (s[n])
        ++n;
    put(s, n);
}

/* The last `limit` bytes of the kernel log ring, oldest first, from the first whole line; `out` gets each run. */
static void msgbuf_tail(uint32_t limit, void (*out)(const char *, size_t))
{
    const struct msgbuf *mb = msgbufp;
    if (!mb || mb->msg_magic != MSG_MAGIC || mb->msg_size <= 0 || mb->msg_size > MAX_MSG_BSIZE || !mb->msg_bufc)
        return;
    const uint32_t size = (uint32_t)mb->msg_size;
    const uint32_t x = (uint32_t)mb->msg_bufx % size;
    const uint32_t n = limit < size ? limit : size;
    uint32_t at = (x + size - n) % size;
    uint32_t left = n;
    /* Start after a partial first line: the window starts mid-line, or it is the whole ring and the ring wrapped. */
    char before = 0;
    if (at != x) {
        before = mb->msg_bufc[(at + size - 1) % size];
    } else if (mb->msg_bufc[x]) {
        before = 'x';
    }
    if (before && before != '\n') {
        while (left && mb->msg_bufc[at] != '\n') {
            at = (at + 1) % size;
            --left;
        }
    }
    while (left) {
        const uint32_t run = (size - at < left) ? size - at : left;
        uint32_t from = at;
        for (uint32_t i = at; i < at + run; ++i) { /* an unwrapped ring is NUL past bufx */
            if (!mb->msg_bufc[i]) {
                if (i > from)
                    out(mb->msg_bufc + from, i - from);
                from = i + 1;
            }
        }
        if (at + run > from)
            out(mb->msg_bufc + from, at + run - from);
        at = (at + run) % size;
        left -= run;
    }
}

/* XNU's panic log and the log after its stackshot, from an embedded panic header at `base` (this boot's or the last). */
static int append_xnu_log(const volatile uint8_t *base, uint32_t bytes)
{
    const volatile struct embedded_panic_header *h = (const volatile struct embedded_panic_header *)base;
    if (h->eph_magic != EMBEDDED_PANIC_MAGIC)
        return 0;
    const uint32_t parts[2][2] = {{h->eph_panic_log_offset, h->eph_panic_log_len},
                                  {h->eph_other_log_offset, h->eph_other_log_len}};
    puts_("\n--- XNU panic log ---\n");
    for (int i = 0; i < 2; ++i) {
        const uint32_t offset = parts[i][0];
        const uint32_t length = parts[i][1];
        if (!offset || offset >= bytes || length > bytes - offset)
            continue;
        for (uint32_t j = 0; j < length && text_at < TEXT_MAX; ++j) {
            const char c = (char)base[offset + j];
            if (c)
                put(&c, 1);
        }
    }
    return 1;
}

/* The panic stackshot of the embedded panic header at `base` (this boot's, or the last one's), into the record's own
 * place: XNU's buffer is reused by the next boot. One line in the text says what happened to it. */
static void keep_stackshot(const volatile uint8_t *base, uint32_t bytes)
{
    const volatile struct embedded_panic_header *h = (const volatile struct embedded_panic_header *)base;
    volatile struct switch_panic_stackshot *k = shot();
    char line[160];
    k->magic = 0;
    drain();
    if (h->eph_magic != EMBEDDED_PANIC_MAGIC)
        return;
    const uint32_t offset = h->eph_stackshot_offset;
    const uint32_t length = h->eph_stackshot_len;
    const uint64_t flags = h->eph_panic_flags;
    if (!(flags & EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_SUCCEEDED) || !offset || !length || offset >= bytes ||
        length > bytes - offset) {
        snprintf(line, sizeof(line), "\n--- no panic stackshot (panic flags 0x%llx) ---\n", (unsigned long long)flags);
    } else if (length > SHOT_MAX) {
        snprintf(line, sizeof(line), "\n--- panic stackshot: %u bytes, over the %u KiB kept: not kept ---\n", length,
                 SHOT_MAX >> 10);
    } else {
        volatile uint8_t *to = region + SHOT_OFFSET;
        for (uint32_t i = 0; i < length; ++i)
            to[i] = base[offset + i];
        k->bytes = length;
        k->crc = crc_of(to, length);
        k->panic_flags = flags;
        k->header_crc = crc_of(k, offsetof(struct switch_panic_stackshot, header_crc));
        drain();
        k->magic = SHOT_MAGIC;
        drain();
        snprintf(line, sizeof(line), "\n--- panic stackshot: %u bytes of %skcdata kept: panic-*.kcdata on Data ---\n",
                 length, (flags & EMBEDDED_PANIC_HEADER_FLAG_STACKSHOT_DATA_COMPRESSED) ? "zlib " : "");
    }
    puts_(line);
}

static void seal(uint32_t phase)
{
    volatile struct switch_panic_console *c = console();
    c->sig = CONSOLE_SIG;
    c->start = text_at;
    c->size = text_at;
    volatile struct switch_panic_record *r = record();
    r->magic = 0;
    drain(); /* a half-written record is never valid */
    r->version = 1;
    r->bytes = sizeof(struct switch_panic_record);
    r->text_bytes = text_at;
    r->text_crc = crc_of(c->text, text_at);
    r->phase = phase;
    r->blocks = (text_at + BLOCK_BYTES - 1) / BLOCK_BYTES;
    for (uint32_t b = 0; b < r->blocks; ++b) {
        const uint32_t at = b * BLOCK_BYTES;
        const uint32_t n = text_at - at < BLOCK_BYTES ? text_at - at : BLOCK_BYTES;
        r->block_crc[b] = crc_of(c->text + at, n);
    }
    r->magic = RECORD_MAGIC;
    r->header_crc = crc_of(r, offsetof(struct switch_panic_record, header_crc));
    drain();
}

static void format_time(char *out, size_t bytes, uint64_t secs)
{
    /* Civil date of a day count (H. Hinnant's days_from_civil inverse). */
    const int64_t z = (int64_t)(secs / 86400) + 719468;
    const int64_t era = z / 146097;
    const uint64_t doe = (uint64_t)(z - era * 146097);
    const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint64_t mp = (5 * doy + 2) / 153;
    const uint64_t d = doy - (153 * mp + 2) / 5 + 1;
    const uint64_t m = mp < 10 ? mp + 3 : mp - 9;
    const uint64_t y = yoe + (uint64_t)era * 400 + (m <= 2);
    const uint64_t s = secs % 86400;
    snprintf(out, bytes, "%04llu-%02llu-%02llu-%02llu%02llu%02llu", (unsigned long long)y, (unsigned long long)m,
             (unsigned long long)d, (unsigned long long)(s / 3600), (unsigned long long)(s / 60 % 60),
             (unsigned long long)(s % 60));
}

/* --- the screen ---------------------------------------------------------------------------------------------- */

#define COLS    160u
#define ROWS    45u
#define LOG_ROW 14u
static const uint32_t kBlack = 0xff000000u, kGrey = 0xffc0c0c0u;

static void glyph(unsigned col, unsigned row, unsigned char ch, uint32_t color)
{
    if (ch < 32 || ch > 126)
        ch = '?';
    const unsigned char *rows = iso_font + (size_t)ch * 16u;
    for (unsigned y = 0; y < 16; ++y) {
        for (unsigned x = 0; x < 8; ++x)
            switch_diag_pixel(screen, col * 8 + x, row * 16 + y, ((unsigned)rows[y] >> x) & 1u ? color : kBlack);
    }
}

static void clear_rows(unsigned first, unsigned last)
{
    for (unsigned x = 0; x < 1280; ++x) {
        volatile uint32_t *column = screen + (uint64_t)(1279 - x) * 720;
        for (unsigned y = first * 16; y < last * 16 && y < 720; ++y)
            column[y] = kBlack;
    }
}

/* `text` from row `row` to row `last` (exclusive), wrapped at COLS: the next free row. */
static unsigned draw(unsigned row, unsigned last, const char *text, size_t length, uint32_t color)
{
    unsigned col = 0;
    for (size_t i = 0; i < length && row < last; ++i) {
        const char c = text[i];
        if (c == '\n') {
            col = 0;
            ++row;
            continue;
        }
        if (c == '\r')
            continue;
        if (col == COLS) {
            col = 0;
            if (++row == last)
                break;
        }
        glyph(col++, row, (unsigned char)(c == '\t' ? ' ' : c), color);
    }
    return col ? row + 1 : row;
}

static unsigned drawz(unsigned row, unsigned last, const char *s, uint32_t color)
{
    size_t n = 0;
    while (s[n])
        ++n;
    return draw(row, last, s, n, color);
}

/* The last rows-worth of lines of the kernel log, into the log area. */
static char tail_text[ROWS * COLS];
static size_t tail_used;

static void tail_out(const char *bytes, size_t length)
{
    for (size_t i = 0; i < length; ++i) {
        if (tail_used == sizeof(tail_text)) { /* keep the newest: drop the oldest half */
            for (size_t j = 0; j < sizeof(tail_text) / 2; ++j)
                tail_text[j] = tail_text[j + sizeof(tail_text) / 2];
            tail_used = sizeof(tail_text) / 2;
        }
        tail_text[tail_used++] = bytes[i];
    }
}

static void draw_log_area(const char *title, const char *text, size_t length)
{
    clear_rows(LOG_ROW, ROWS - 1);
    drawz(LOG_ROW, LOG_ROW + 1, title, SWITCH_GREEN);
    /* As many of the last lines as fit. */
    size_t start = length;
    unsigned rows = 0;
    while (start > 0) {
        size_t line = start - 1;
        while (line > 0 && text[line - 1] != '\n')
            --line;
        const size_t chars = start - line;
        const unsigned need = chars > COLS ? (unsigned)((chars - 2) / COLS + 1) : 1; /* its newline takes no column */
        if (rows + need > ROWS - 2 - LOG_ROW)
            break;
        rows += need;
        start = line;
    }
    draw(LOG_ROW + 1, ROWS - 1, text + start, length - start, kGrey);
}

static int watchdog_panic; /* the panic string is a watchdog timeout (switch_panic_begin) */

static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static void draw_status(void)
{
    char line[COLS + 1];
    if (save_on && region && reboot_s && watchdog_panic && !reboot_arg) {
        snprintf(line, sizeof(line),
                 "Watchdog panic: saved in RAM. Warm reset %us after the panic; iPadOS then writes "
                 "/private/var/Switch/panic-*.log and .kcdata (the stackshot).",
                 reboot_s);
    } else if (save_on && region && reboot_s) {
        snprintf(line, sizeof(line),
                 "Saved in RAM. Warm reset %us after the panic: hekate writes SD:/L4T_panic.txt, iPadOS "
                 "/private/var/Switch/panic-*.log.",
                 reboot_s);
    } else if (save_on && region) {
        snprintf(line, sizeof(line), "Saved in RAM (no reset). Holding POWER loses it.");
    } else {
        snprintf(line, sizeof(line), "Not saved (%s). Hold POWER to turn off.", save_off_reason);
    }
    clear_rows(ROWS - 1, ROWS);
    drawz(ROWS - 1, ROWS, line, save_on && region ? SWITCH_GREEN : SWITCH_RED);
    drain();
}

static void draw_screen(void)
{
    if (!screen_taken)
        return;
    for (uint32_t i = 0; i < SWITCH_FB_BYTES / 4; ++i)
        screen[i] = kBlack;
    char title[COLS + 1];
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(panic_at, &ns);
    snprintf(title, sizeof(title), "XNU PANIC   cpu %u   %llu.%03llu s after boot", panic_cpu, ns / 1000000000ULL,
             ns / 1000000ULL % 1000);
    drawz(0, 1, title, SWITCH_RED);
    drawz(1, LOG_ROW - 1, panic_line, SWITCH_WHITE);
    tail_used = 0;
    msgbuf_tail(sizeof(tail_text), tail_out);
    draw_log_area("Kernel log, last lines:", tail_text, tail_used);
    draw_status();
}

/* --- reset ------------------------------------------------------------------------------------------------- */

static void arm_watchdog(unsigned seconds)
{
    if (!tmr || !seconds)
        return;
    if (seconds > 255)
        seconds = 255;
    tmr[TMR5_PTV / 4] = (1u << 31) | (1u << 30) | 250000u; /* TMR5: enabled, periodic, 250 ms */
    tmr[WDT0_CFG / 4] = 5u | (seconds << 4) | (1u << 15);  /* WDT0: source TMR5, period, PMC2CAR reset */
    tmr[WDT0_CMD / 4] = 1u;                                /* START_COUNTER */
    drain();
}

/* Linux tegra_wdt_stop: the unlock pattern, DISABLE_COUNTER, TMR5 off. */
static void disarm_watchdog(void)
{
    if (!tmr)
        return;
    tmr[WDT0_UNLOCK / 4] = 0xc45au;
    tmr[WDT0_CMD / 4] = 2u;
    tmr[TMR5_PTV / 4] = 0;
    drain();
}

void switch_panic_save_watchdog(unsigned seconds)
{
    disarm_watchdog();
    arm_watchdog(seconds);
}

static void __attribute__((noreturn)) pmc_reset(void)
{
    pmc[PMC_SCRATCH0 / 4] = pmc[PMC_SCRATCH0 / 4] & ~PMC_SCRATCH0_MODES; /* no warmboot, RCM or hekate mode request */
    drain();
    pmc[PMC_CNTRL / 4] = pmc[PMC_CNTRL / 4] | PMC_MAIN_RST;
    for (;;)
        __asm__ volatile("wfe" ::: "memory");
}

/* A record a panic sealed, at region. */
static int record_sealed(void)
{
    volatile struct switch_panic_record *r = record();
    volatile struct switch_panic_console *c = console();
    return r->magic == RECORD_MAGIC && r->version == 1 && r->bytes == sizeof(struct switch_panic_record) &&
           r->header_crc == crc_of(r, offsetof(struct switch_panic_record, header_crc)) && r->text_bytes <= TEXT_MAX &&
           r->blocks == (r->text_bytes + BLOCK_BYTES - 1) / BLOCK_BYTES && c->sig == CONSOLE_SIG &&
           c->size == r->text_bytes;
}

/* --- hooks ------------------------------------------------------------------------------------------------- */

void switch_panic_save_init(void)
{
    if (switch_native_profile_active())
        screen = (volatile uint32_t *)ml_io_map_wcomb((vm_offset_t)SWITCH_FB_PA, (vm_size_t)SWITCH_FB_BYTES);
    pmc = (volatile uint32_t *)ml_io_map((vm_offset_t)PMC_PA, 4096);
    if (pmc)
        last_reset_status = pmc[PMC_RST_STATUS / 4];
    save_on = 1;
    for (uint64_t pa = REGION_PA; pa < REGION_PA + REGION_BYTES; pa += PAGE_SIZE) {
        if (!switch_ram_reserved((ppnum_t)atop_64(pa))) {
            save_off_reason = "0xb0000000 is not reserved RAM";
            return;
        }
    }
    /* Always: a watchdog panic resets even with the panic screen held. */
    tmr = (volatile uint32_t *)ml_io_map((vm_offset_t)TMR_PA, 4096);
    region = (volatile uint8_t *)ml_io_map_wcomb((vm_offset_t)REGION_PA, REGION_BYTES);
    if (!pmc || !region || !tmr) {
        region = NULL;
        save_off_reason = "mapping failed";
        return;
    }
    switch_power_trail_init(region + TRAIL_OFFSET, TRAIL_BYTES);
    last_reset = last_reset_status & 7;
    if (pmc[PMC_SCRATCH37 / 4] == TPAN)
        pmc[PMC_SCRATCH37 / 4] = 0; /* hekate clears it; never leave it for later */
    /* The last boot's record, checked now; written to Data by kern_exec.c (switch_panic_save_pending). */
    if (!record_sealed())
        return;
    volatile struct switch_panic_record *r = record();
    volatile struct switch_panic_console *c = console();
    pending = 1;
    for (uint32_t b = 0; b < r->blocks; ++b) {
        const uint32_t at = b * BLOCK_BYTES;
        const uint32_t n = r->text_bytes - at < BLOCK_BYTES ? r->text_bytes - at : BLOCK_BYTES;
        if (r->block_crc[b] != crc_of(c->text + at, n))
            ++pending_damaged;
    }
    /* Reset before the panic log was appended (by the watchdog): it is still in the last boot's XNU buffer, which XNU
     * clears only later (check_for_panic_log). Appended as DRAM kept it, and sealed again (the damage count stays). */
    text_at = r->text_bytes;
    if (r->phase == 1 && append_xnu_log(region, XNU_LOG_BYTES)) {
        keep_stackshot(region, XNU_LOG_BYTES);
        seal(2);
    }
}

int switch_panic_save_xnu_buffer(uintptr_t *base, unsigned int *bytes)
{
    if (!save_on || !region)
        return 0;
    *base = (uintptr_t)region;
    *bytes = XNU_LOG_BYTES;
    return 1;
}

int switch_panic_begin(const char *format, va_list *args, unsigned long caller, int take_screen)
{
    if (__atomic_exchange_n(&begun, 1u, __ATOMIC_ACQ_REL))
        return screen_taken;
    panic_at = mach_absolute_time();
    panic_cpu = (unsigned)cpu_number();
    /* NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_copy initializes it */
    va_list copy;
    va_copy(copy, *args);
    const int length = vsnprintf(panic_line, sizeof(panic_line), format, copy);
    va_end(copy);
    if (length < 0)
        panic_line[0] = 0;
    /* watchdogd's (AppleARMWatchdogTimer's IOWatchdogUserClient) and the kernel's own check of watchdogd. */
    watchdog_panic = starts_with(panic_line, "userspace watchdog timeout:") ||
                     starts_with(panic_line, "watchdog timeout: no checkins from ");
    if (watchdog_panic && !reboot_arg)
        reboot_s = WATCHDOG_REBOOT_S;
    if (get_system_inshutdown() && !reboot_arg && !reboot_s)
        reboot_s = SHUTDOWN_REBOOT_S;
    if (!switch_display_is_on() && !reboot_arg && !reboot_s)
        reboot_s = DARK_REBOOT_S; /* nothing to photograph */
    screen_taken = take_screen && screen;
    if (save_on && region) {
        clock_sec_t secs = 0;
        clock_nsec_t nsecs = 0;
        switch_clock_calendar_nowait(&secs, &nsecs);
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(panic_at, &ns);
        char when[32];
        char head[256];
        format_time(when, sizeof(when), secs);
        text_at = 0;
        shot()->magic = 0; /* the last panic's, should it still be here */
        snprintf(head, sizeof(head),
                 "SwitchPanic record 1: %s UTC, %llu.%03llu s after boot, cpu %u, caller 0x%lx\npanic: ", when,
                 ns / 1000000000ULL, ns / 1000000ULL % 1000, panic_cpu, caller);
        puts_(head);
        puts_(panic_line);
        puts_("\n\n--- kernel log, last 64 KiB (msgbuf) ---\n");
        msgbuf_tail(MSGBUF_TAIL, put);
        volatile struct switch_panic_record *r = record();
        r->cpu = panic_cpu;
        r->caller = caller;
        r->uptime_ns = ns;
        r->time_s = secs;
        r->time_ns = nsecs;
        seal(1);
        if (hekate_on)
            pmc[PMC_SCRATCH37 / 4] = TPAN;
        arm_watchdog(reboot_s ? reboot_s + 15 : 0); /* the reset below, should the panic path hang */
    }
    draw_screen();
    return screen_taken;
}

void switch_panic_halted(void)
{
    if (!begun || !screen_taken)
        return;
    void (*const display)(const char *) = switch_display_panic_hook;
    if (display)
        display(""); /* the display puts window A back again, should a flip have moved it */
    draw_screen();
}

void switch_panic_logged(void)
{
    static uint32_t once;
    if (!begun || __atomic_exchange_n(&once, 1u, __ATOMIC_ACQ_REL))
        return;
    const volatile uint8_t *xnu = (const volatile uint8_t *)gPanicBase;
    if (save_on && region && xnu == region && record()->magic == RECORD_MAGIC && append_xnu_log(xnu, XNU_LOG_BYTES)) {
        keep_stackshot(xnu, XNU_LOG_BYTES);
        seal(2);
    }
    if (screen_taken && panic_info && panic_info->eph_panic_log_offset && panic_info->eph_panic_log_len < gPanicSize) {
        /* The panic log from its "Panicked task" line: the backtrace. */
        const char *log = (const char *)gPanicBase + panic_info->eph_panic_log_offset;
        const uint32_t length = panic_info->eph_panic_log_len;
        uint32_t from = 0;
        static const char marker[] = "Panicked task ";
        for (uint32_t i = 0; i + sizeof(marker) < length && !from; ++i) {
            uint32_t n = 0;
            while (n + 1 < sizeof(marker) && log[i + n] == marker[n])
                ++n;
            if ((i == 0 || log[i - 1] == '\n') && n + 1 == sizeof(marker))
                from = i;
        }
        draw_log_area("XNU panic log:", log + from, length - from);
        draw_status();
    }
    /* Still on XNU's verbose text console (a panic before the GUI took the screen): the panic log has scrolled the
     * panic string off the top, so it is printed again under the backtrace, where a photo of the screen finds it. */
    if (!screen_taken && panic_line[0])
        kdb_printf_unbuffered("\npanic (cpu %u): %s\n", panic_cpu, panic_line);
    if (!save_on || !region || !reboot_s)
        return;
    uint64_t deadline = 0;
    nanoseconds_to_absolutetime((uint64_t)reboot_s * 1000000000ULL, &deadline);
    deadline += panic_at;
    while (mach_absolute_time() < deadline)
        __asm__ volatile("isb" ::: "memory");
    switch_power_trail_flag_now(SWITCH_MARK_FLAG_PANIC_RESET);
    pmc_reset();
}

int switch_panic_save_ready(void)
{
    return save_on && region;
}

int switch_panic_save_early_pending(volatile uint8_t *ramoops)
{
    volatile uint8_t *const mapped = region;
    region = ramoops;
    const int sealed = record_sealed();
    region = mapped;
    return sealed;
}

void switch_panic_save_early(const char *line, unsigned long caller, volatile uint8_t *ramoops, volatile uint32_t *fb,
                             volatile uint32_t *pmc_page)
{
    region = ramoops;
    screen = fb;
    pmc = pmc_page;
    save_on = 1;
    begun = 1;
    screen_taken = fb != NULL;
    reboot_s = EARLY_REBOOT_S;
    snprintf(panic_line, sizeof(panic_line), "%s", line);
    char head[160];
    snprintf(head, sizeof(head),
             "SwitchPanic record 1: early boot, before the kernel's console\npanic(cpu 0 caller 0x%lx): ", caller);
    text_at = 0;
    shot()->magic = 0;
    puts_(head);
    puts_(panic_line);
    puts_("\n");
    volatile struct switch_panic_record *r = record();
    r->cpu = 0;
    r->caller = caller;
    r->uptime_ns = 0;
    r->time_s = 0;
    r->time_ns = 0;
    seal(1);
    /* hekate saves the record to SD:/L4T_panic.txt on this magic: a RELEASE kernel has nothing that files it. */
    pmc[PMC_SCRATCH37 / 4] = TPAN;
    if (screen_taken) {
        for (uint32_t i = 0; i < SWITCH_FB_BYTES / 4; ++i)
            screen[i] = kBlack;
        drawz(0, 1, "XNU PANIC   early boot, before the kernel's console", SWITCH_RED);
        drawz(1, ROWS - 1, panic_line, SWITCH_WHITE);
        char status[COLS + 1];
        snprintf(status, sizeof(status),
                 "Saved in RAM. Warm reset in %us: hekate shows it and writes SD:/L4T_panic.txt.", reboot_s);
        drawz(ROWS - 1, ROWS, status, SWITCH_GREEN);
        drain();
    }
    uint64_t deadline = 0;
    nanoseconds_to_absolutetime((uint64_t)reboot_s * 1000000000ULL, &deadline);
    deadline += mach_absolute_time();
    while (mach_absolute_time() < deadline)
        __asm__ volatile("yield" ::: "memory");
    pmc_reset();
}

void switch_panic_save_thermal(int sensor, int millic)
{
    thermal_sensor = sensor;
    thermal_millic = millic;
}

/* One line for every boot, as an error when the last boot did not end by a shut down, a restart or a panic it saved;
 * and the report kern_exec.c files for those ends. */
static const char *reset_report(char *path, size_t path_bytes)
{
    const enum switch_reset_cause cause = switch_reset_cause(last_reset_status);
    const enum switch_thermal_sensor thermal = (enum switch_thermal_sensor)thermal_sensor;
    char thermal_text[96] = "";
    if (thermal != SWITCH_THERMAL_NONE) {
        snprintf(thermal_text, sizeof(thermal_text), "; thermal shutdown %s %d.%d C",
                 switch_thermal_sensor_name(thermal), thermal_millic / 1000, thermal_millic % 1000 / 100);
    }
    const int report = switch_reset_files_report(cause, pending, thermal);
    if (report) {
        os_log_error(OS_LOG_DEFAULT, "SwitchReset: last boot ended by %s (PMC_RST_STATUS %#x)%s%s",
                     switch_reset_cause_name(cause), last_reset_status, thermal_text,
                     pending ? "; its panic is filed" : "");
    } else {
        os_log(OS_LOG_DEFAULT, "SwitchReset: last boot ended by %s (PMC_RST_STATUS %#x)%s%s",
               switch_reset_cause_name(cause), last_reset_status, thermal_text, pending ? "; its panic is filed" : "");
    }
    if (!report)
        return NULL;
    char when[32];
    clock_sec_t secs = 0;
    clock_usec_t usecs = 0;
    clock_get_calendar_microtime(&secs, &usecs);
    format_time(when, sizeof(when), (uint64_t)secs);
    snprintf(path, path_bytes, "/private/var/Switch/reset-%s.log", when);
    const char *story = "The SoC watchdog reset a hung console before any panic was recorded.";
    if (thermal != SWITCH_THERMAL_NONE) {
        story = "SwitchThermal shut the console down in order: it crossed its software thermal shutdown threshold "
                "(SwitchThermalLimits.h).";
    } else if (cause == SWITCH_RESET_THERMTRIP) {
        story = "SOC_THERM's hardware thermal trip reset the SoC: the software thermal shutdown below it did not get "
                "there first.";
    }
    snprintf(report_text, sizeof(report_text),
             "Switch reset report, filed %s UTC at the next boot\n"
             "The last boot ended by: %s (PMC_RST_STATUS %#x)%s\n"
             "%s\n",
             when, switch_reset_cause_name(cause), last_reset_status, thermal_text, story);
    report_pending = 1;
    return report_text;
}

const char *switch_panic_save_pending(uint32_t *bytes, char *path, size_t path_bytes)
{
    switch_power_trail_report();
    static const char *const resets[8] = {"power-on", "watchdog", "thermal sensor", "software (PMC)",
                                          "LP0 wake", "AOTAG",    "unknown 6",      "unknown 7"};
    if (!pending && pmc) {
        const char *report = reset_report(path, path_bytes);
        if (report) {
            *bytes = (uint32_t)strnlen(report, sizeof(report_text));
            return report;
        }
    } else if (pmc) {
        (void)reset_report(path, path_bytes);
    }
    if (!save_on || !region) {
        if (save_on)
            os_log(OS_LOG_DEFAULT, "SwitchPanicSave: off: %s", save_off_reason);
        return NULL;
    }
    if (!pending) {
        os_log(OS_LOG_DEFAULT,
               "SwitchPanicSave: armed (%u MiB at %#llx, warm reset %u s after a panic), last reset %s, no saved "
               "panic",
               REGION_BYTES >> 20, (unsigned long long)REGION_PA, reboot_s, last_reset < 8 ? resets[last_reset] : "?");
        return NULL;
    }
    char when[32];
    format_time(when, sizeof(when), record()->time_s);
    snprintf(path, path_bytes, "/private/var/Switch/panic-%s%s.log", when, pending_damaged ? "-damaged" : "");
    os_log(OS_LOG_DEFAULT,
           "SwitchPanicSave: armed, last reset %s, saved panic of %s UTC: %u bytes, %d of %u blocks damaged",
           last_reset < 8 ? resets[last_reset] : "?", when, record()->text_bytes, pending_damaged, record()->blocks);
    *bytes = record()->text_bytes;
    return (const char *)(uintptr_t)console()->text;
}

const void *switch_panic_save_pending_stackshot(uint32_t *bytes)
{
    if (!save_on || !region || !pending || report_pending)
        return NULL;
    const volatile struct switch_panic_stackshot *k = shot();
    if (k->magic != SHOT_MAGIC || k->header_crc != crc_of(k, offsetof(struct switch_panic_stackshot, header_crc)) ||
        !k->bytes || k->bytes > SHOT_MAX)
        return NULL;
    const volatile uint8_t *data = region + SHOT_OFFSET;
    if (k->crc != crc_of(data, k->bytes)) {
        os_log(OS_LOG_DEFAULT, "SwitchPanicSave: the saved panic stackshot is damaged (%u bytes): not written",
               k->bytes);
        return NULL;
    }
    *bytes = k->bytes;
    return (const void *)(uintptr_t)data;
}

void switch_panic_save_done(int error, const char *path)
{
    if (report_pending) {
        report_pending = 0;
        os_log(OS_LOG_DEFAULT, "SwitchReset: %s %s", path, error ? "not written" : "written");
        return;
    }
    if (!pending)
        return;
    if (error) {
        os_log(OS_LOG_DEFAULT, "SwitchPanicSave: writing %s failed: %d (kept for the next boot)", path, error);
        return;
    }
    pending = 0;
    record()->magic = 0;
    shot()->magic = 0;
    console()->sig = 0;
    drain();
    os_log(OS_LOG_DEFAULT, "SwitchPanicSave: %s written", path);
}
