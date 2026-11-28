/* Deliberately small source-only diagnostic for the one-CPU native experiment, debug kernels only.
 * Compile-time + explicit boot-argument gated; no firmware or security services.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include "SwitchDiag.h"
#include "SwitchDiagDisplay.h"
#include "SwitchDiagHud.h"

#if DEVELOPMENT || DEBUG
extern int vsnprintf(char *, size_t, const char *, va_list);
extern void panic_spin_forever(void) __attribute__((noreturn));

static volatile uint32_t *framebuffer;
/* Fixed RAM text history. No allocation, firmware, UART or storage access. */
#define CONSOLE_ROWS    24u
#define CONSOLE_COLUMNS 104u
static char console_lines[CONSOLE_ROWS][CONSOLE_COLUMNS + 1];
static unsigned console_enabled, console_busy, console_head, console_column, console_used;
static uint64_t console_last_draw;
static unsigned hud_hidden;

static void sync_pixels(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static void text(unsigned y, const char *message, unsigned limit, uint32_t color)
{
    if (!framebuffer || switch_native_text_console_active())
        return;
    (void)switch_diag_text(framebuffer, 16, y, 704, message, limit, color);
}

static void hex_line(unsigned y, const char *label, uint64_t value)
{
    char line[64];
    unsigned n = 0;
    while (label[n] && n < sizeof(line) - 19) {
        line[n] = label[n];
        ++n;
    }
    line[n++] = '0';
    line[n++] = 'X';
    for (unsigned i = 0; i < 16; ++i) {
        unsigned digit = (unsigned)((value >> (60 - 4 * i)) & 15);
        line[n++] = "0123456789ABCDEF"[digit];
    }
    line[n] = 0;
    text(y, line, sizeof(line), SWITCH_WHITE);
}

int switch_diag_console_active(void)
{
    return console_enabled && switch_diag_record.ready && !switch_diag_record.panicked;
}

void switch_diag_graphics_takeover(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (!switch_diag_record.panicked && framebuffer) {
        hud_hidden = 1;
        switch_diag_record.ready = 0;
    }
    (void)ml_set_interrupts_enabled(previous);
}

void switch_diag_graphics_release(void)
{
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (hud_hidden && !switch_diag_record.panicked) {
        hud_hidden = 0;
        switch_diag_record.ready = 1;
    }
    (void)ml_set_interrupts_enabled(previous);
}

static void console_blank(unsigned row)
{
    for (unsigned i = 0; i < CONSOLE_COLUMNS; i++)
        console_lines[row][i] = ' ';
    console_lines[row][CONSOLE_COLUMNS] = 0;
}

/* Called with interrupts disabled on the already-required single CPU. */
static void console_flush(void)
{
    uint64_t now = 0;
    uint64_t freq = 0;
    __asm__ volatile("mrs %0,cntpct_el0" : "=r"(now));
    __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(freq));
    if (console_last_draw && freq && now - console_last_draw < freq / 5)
        return;
    console_last_draw = now;
    unsigned first = console_used == CONSOLE_ROWS ? (console_head + 1) % CONSOLE_ROWS : 0;
    for (unsigned row = 0; row < CONSOLE_ROWS; row++) {
        const char *line = row < console_used ? console_lines[(first + row) % CONSOLE_ROWS] : NULL;
        if (line) {
            text(180 + row * 20, line, CONSOLE_COLUMNS + 1, SWITCH_WHITE);
        } else {
            for (unsigned y = 180 + row * 20; y < 194 + row * 20; y++) {
                for (unsigned x = 16; x < 1264; x++)
                    switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
            }
        }
    }
    sync_pixels();
}

void switch_diag_console_putc(char c)
{
    if (!switch_diag_console_active())
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    if (console_busy) {
        (void)ml_set_interrupts_enabled(previous);
        return;
    }
    console_busy = 1;
    if (c != '\r') {
        if (c == '\n' || console_column == CONSOLE_COLUMNS) {
            console_head = (console_head + 1) % CONSOLE_ROWS;
            if (console_used < CONSOLE_ROWS)
                console_used++;
            console_blank(console_head);
            console_column = 0;
            console_flush();
        }
        if (c != '\n') {
            if (c == '\b') {
                if (console_column)
                    console_lines[console_head][--console_column] = ' ';
            } else {
                char shown = (unsigned char)c >= 32 && (unsigned char)c < 127 ? c : '?';
                if (c == '\t')
                    shown = ' ';
                console_lines[console_head][console_column++] = shown;
            }
        }
    }
    console_busy = 0;
    (void)ml_set_interrupts_enabled(previous);
}

void switch_diag_process_event(int pid, const char *name, const char *event, int status)
{
    (void)pid;
    (void)name;
    (void)event;
    (void)status;
}

void switch_diag_hud_init(const boot_args *args, int cpus)
{
    if (cpus != 1)
        return; /* Four-core native uses only the stock video console. */
    if (!switch_diag_video_matches(args->Video.v_baseAddr, args->Video.v_display, args->Video.v_rowBytes,
                                   args->Video.v_width, args->Video.v_height, args->Video.v_depth))
        return;
    /* io_map's bootstrap path is available once arm_vm_init returned. */
    vm_offset_t mapped = ml_io_map((vm_offset_t)SWITCH_FB_PA, (vm_size_t)SWITCH_FB_BYTES);
    if (!mapped)
        return;
    framebuffer = (volatile uint32_t *)mapped;
    switch_diag_record.magic = UINT64_C(0x53574348584e5531); /* SWCHXNU1 */
    switch_diag_record.framebuffer_va = mapped;
    switch_diag_record.ready = 1;
    switch_diag_clear(framebuffer);
    text(24, "SWITCH NATIVE XNU DIAGNOSTICS", 104, SWITCH_WHITE);
    text(64, "XNU VM INITIALIZED", 104, SWITCH_GREEN);
    text(680, "KERNEL DIAGNOSTICS ONLY - USERSPACE IS NOT ESTABLISHED", 104, SWITCH_WHITE);
    switch_diag_stage("arm_vm_init returned");
}

void switch_diag_stage(const char *message)
{
    if (!switch_diag_record.ready || switch_diag_record.panicked)
        return;
    boolean_t previous = ml_set_interrupts_enabled(FALSE);
    unsigned n = 0;
    for (; message[n] && n < sizeof(switch_diag_record.stage) - 1; ++n)
        switch_diag_record.stage[n] = message[n];
    switch_diag_record.stage[n] = 0;
    ++switch_diag_record.stages;
    for (unsigned y = 100; y < 120; ++y) {
        for (unsigned x = 16; x < 1264; ++x)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    text(100, switch_diag_record.stage, sizeof(switch_diag_record.stage), SWITCH_GREEN);
    sync_pixels();
    (void)ml_set_interrupts_enabled(previous);
}

__attribute__((no_stack_protector)) void switch_diag_hud_panic(const char *format, va_list *args, unsigned long caller)
{
    if (hud_hidden && framebuffer) {
        hud_hidden = 0;
        switch_diag_record.ready = 1;
    }
    if (!switch_diag_record.ready)
        return;
    __asm__ volatile("msr daifset, #15" ::: "memory");
    /* One CPU is required at init. A recursive formatter fault must not redraw. */
    if (switch_diag_record.panicked)
        panic_spin_forever();
    switch_diag_record.panicked = 1;
    for (unsigned y = 140; y < 660; y++) {
        for (unsigned x = 16; x < 1264; x++)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    switch_diag_record.caller = caller;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(switch_diag_record.current_el));
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(switch_diag_record.sctlr));
    __asm__ volatile("mrs %0, tcr_el1" : "=r"(switch_diag_record.tcr));
    __asm__ volatile("mrs %0, esr_el1" : "=r"(switch_diag_record.sampled_esr));
    __asm__ volatile("mrs %0, far_el1" : "=r"(switch_diag_record.sampled_far));
    /* Draw the header before formatting so a bad panic argument cannot leave a success banner. */
    text(144, "XNU PANIC - EXECUTION STOPPED", 104, SWITCH_RED);
    hex_line(184, "CALLER=", caller);
    hex_line(204, "CURRENTEL=", switch_diag_record.current_el);
    hex_line(224, "SCTLR=", switch_diag_record.sctlr);
    hex_line(244, "TCR=", switch_diag_record.tcr);
    hex_line(264, "SAMPLED ESR=", switch_diag_record.sampled_esr);
    hex_line(284, "SAMPLED FAR=", switch_diag_record.sampled_far);
    sync_pixels();
    /* NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_copy initializes it */
    va_list copy;
    va_copy(copy, *args);
    int count = vsnprintf(switch_diag_record.panic_text, sizeof(switch_diag_record.panic_text), format, copy);
    va_end(copy);
    switch_diag_record.truncated = count < 0 || (unsigned)count >= sizeof(switch_diag_record.panic_text);
    if (switch_diag_text(framebuffer, 16, 324, 628, switch_diag_record.panic_text,
                         sizeof(switch_diag_record.panic_text), SWITCH_WHITE))
        switch_diag_record.truncated = 1;
    if (switch_diag_record.truncated)
        text(644, "PANIC TEXT TRUNCATED", 104, SWITCH_RED);
    for (unsigned y = 680; y < 700; ++y) {
        for (unsigned x = 16; x < 1264; ++x)
            switch_diag_pixel(framebuffer, x, y, SWITCH_BACKGROUND);
    }
    text(680, "HALTED ON PANIC - PHOTOGRAPH THIS SCREEN", 104, SWITCH_RED);
    sync_pixels();
    panic_spin_forever();
}

#if defined(XNU_SWITCH_VERIFIED_ROOT) && XNU_SWITCH_VERIFIED_ROOT
#include "SwitchRootDiag.h"
#endif
#endif
