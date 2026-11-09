/* What every kernel config keeps of the board's diagnostics: the early exception screen start.s arms, the native
 * profile's panic entry into SwitchPanicSave.c, and the display hand-over around the GUI. The single-CPU HUD and its
 * observers are debug kernels' only (SwitchDiagHud.c).
 */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <kern/misc_protos.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include "SwitchSD.h"
#include "SwitchDiag.h"
#include "SwitchDiagDisplay.h"
#include "SwitchDiagHud.h"
#include "SwitchPanicSave.h"

extern int vsnprintf(char *, size_t, const char *, va_list);

struct switch_diag_state switch_diag_record;
static unsigned graphics_owns_display;
static unsigned native_text_console, native_profile;

/* Only the assembly entry can arm this after validating the fixed contract.
 * Last stage and fault fields remain available even if the display cannot draw.
 * [enabled, stage, recursive-fault guard, vector, ESR, FAR, ELR, SCTLR]. */
/* NOLINTNEXTLINE(misc-use-internal-linkage): osfmk/arm64/start.s uses it by name */
volatile uint64_t switch_early_record[8];
/* NOLINTNEXTLINE(misc-use-internal-linkage): osfmk/arm64/start.s uses it by name */
unsigned char switch_early_stack[4096] __attribute__((aligned(16)));
extern const unsigned char ExceptionVectorsBase[];

__attribute__((no_stack_protector)) static void early_hex(unsigned y, const char *label, uint64_t value)
{
    char line[80];
    unsigned n = 0;
    while (label[n] && n < sizeof(line) - 19) {
        line[n] = label[n];
        ++n;
    }
    line[n++] = '0';
    line[n++] = 'X';
    for (unsigned i = 0; i < 16; ++i)
        line[n++] = "0123456789ABCDEF"[(value >> (60 - 4 * i)) & 15];
    line[n] = 0;
    (void)switch_diag_text((volatile uint32_t *)(uintptr_t)SWITCH_FB_PA, 16, y, 704, line, sizeof(line), SWITCH_WHITE);
}

// Records the last early stage reached (shown as LAST STAGE on an early exception or panic screen). Nothing is drawn:
// a normal boot leaves the screen to XNU's console.
__attribute__((no_stack_protector)) void switch_diag_early_stage(unsigned stage, const char *message)
{
    (void)message;
    if (!switch_early_record[0] || switch_early_record[2] || stage < 7 || stage > 21)
        return;
    switch_early_record[1] = stage;
}

__attribute__((no_stack_protector)) void switch_diag_early_leave(void)
{
    if (!switch_early_record[0])
        return;
    /* The new TTBR1 is active; old TTBR0 still maps the framebuffer. Restore
     * normal exception vectors and disable early rendering BEFORE TTBR0 dies. */
    switch_early_record[0] = 0;
    __asm__ volatile("dsb sy\nmsr vbar_el1, %0\nisb" ::"r"((uintptr_t)ExceptionVectorsBase) : "memory");
}

__attribute__((noreturn, no_stack_protector)) void switch_diag_early_fault(void)
{
    if (switch_early_record[0]) {
        volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)SWITCH_FB_PA;
        // Erase only the bounded early-C text area, preserving loader status.
        for (unsigned y = 320; y < 620; ++y) {
            for (unsigned x = 16; x < 1264; ++x)
                switch_diag_pixel(pixels, x, y, SWITCH_BACKGROUND);
        }
        (void)switch_diag_text(pixels, 16, 340, 704, "EARLY XNU EXCEPTION - HALTED", 104, SWITCH_RED);
        early_hex(380, "VECTOR=", switch_early_record[3]);
        early_hex(400, "LAST STAGE=", switch_early_record[1]);
        early_hex(420, "ESR=", switch_early_record[4]);
        early_hex(440, "FAR=", switch_early_record[5]);
        early_hex(460, "ELR=", switch_early_record[6]);
        early_hex(480, "SCTLR=", switch_early_record[7]);
        (void)switch_diag_text(pixels, 16, 540, 704, "PHOTOGRAPH THIS SCREEN - HOLD POWER TO TURN OFF", 104,
                               SWITCH_RED);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    for (;;)
        __asm__ volatile("wfe" ::: "memory");
}

int switch_native_text_console_active(void)
{
    return native_text_console;
}

int switch_native_profile_active(void)
{
    return native_profile;
}

/* NativeLegacyDisplay reserves scanout just before copying a real surface. */
/* NOLINTNEXTLINE(misc-use-internal-linkage): kits/qemu's LegacyDisplayProbe calls it (weak import) */
int boot_progress_prepare_scanout(void)
{
    if (switch_diag_record.panicked)
        return 0;
    if (native_text_console) {
        if (!graphics_owns_display) {
            PE_initialize_console(NULL, kPEReleaseScreen);
            graphics_owns_display = 1;
        }
        return !switch_diag_record.panicked;
    }
    switch_diag_graphics_takeover();
    return 1;
}

/* NOLINTNEXTLINE(misc-use-internal-linkage): kits/qemu's LegacyDisplayProbe calls it (weak import) */
void boot_progress_cancel_scanout(void)
{
    if (native_text_console) {
        if (graphics_owns_display && !switch_diag_record.panicked) {
            graphics_owns_display = 0;
            PE_initialize_console(NULL, kPEAcquireScreen);
        }
        return;
    }
    switch_diag_graphics_release();
}

void switch_diag_init(const boot_args *args)
{
    const int cpus = ml_get_max_cpu_number() + 1;
    if (!switch_native_cpu_profile_matches(cpus))
        return;
    /* The native profile belongs exclusively to XNU's video console (text mode) or to the boot logo (graphics mode).
     * Keep observations in RAM, but never map or arm the custom HUD renderer. */
    if (args->Video.v_baseAddr == SWITCH_FB_PA && args->Video.v_display <= 1 && args->Video.v_rowBytes == 2880 &&
        args->Video.v_width == 720 && args->Video.v_height == 1280 && args->Video.v_depth == 0x320) {
        native_profile = 1;
        native_text_console = args->Video.v_display == 0;
        switch_diag_record.magic = UINT64_C(0x53574348584e5531);
        return;
    }
    switch_diag_hud_init(args, cpus);
}

void (*volatile switch_display_panic_hook)(const char *message);

__attribute__((no_stack_protector)) void switch_diag_panic(const char *format, va_list *args, unsigned long caller)
{
    void (*const display)(const char *) = switch_display_panic_hook;
    if (display)
        display(format);
    if (switch_early_record[0]) {
        __asm__ volatile("msr daifset, #15" ::: "memory");
        if (switch_early_record[2] || switch_diag_record.panicked) {
            for (;;)
                __asm__ volatile("wfe" ::: "memory");
        }
        switch_early_record[2] = 1;
        switch_diag_record.panicked = 1;
        switch_diag_record.caller = caller;
        volatile uint32_t *pixels = (volatile uint32_t *)(uintptr_t)SWITCH_FB_PA;
        for (unsigned y = 320; y < 620; ++y) {
            for (unsigned x = 16; x < 1264; ++x)
                switch_diag_pixel(pixels, x, y, SWITCH_BACKGROUND);
        }
        (void)switch_diag_text(pixels, 16, 340, 704, "EARLY XNU PANIC - HALTED", 104, SWITCH_RED);
        early_hex(380, "LAST STAGE=", switch_early_record[1]);
        early_hex(400, "CALLER=", caller);
        __asm__ volatile("dsb sy" ::: "memory");
        /* NOLINTNEXTLINE(cppcoreguidelines-init-variables): va_copy initializes it */
        va_list copy;
        va_copy(copy, *args);
        int count = vsnprintf(switch_diag_record.panic_text, sizeof(switch_diag_record.panic_text), format, copy);
        va_end(copy);
        switch_diag_record.truncated = count < 0 || (unsigned)count >= sizeof(switch_diag_record.panic_text);
        if (switch_diag_text(pixels, 16, 440, 620, switch_diag_record.panic_text, sizeof(switch_diag_record.panic_text),
                             SWITCH_WHITE))
            switch_diag_record.truncated = 1;
        __asm__ volatile("dsb sy" ::: "memory");
        for (;;)
            __asm__ volatile("wfe" ::: "memory");
    }
    if (native_profile) {
        const int first = !switch_diag_record.panicked;
        switch_diag_record.panicked = 1;
        /* Once the GUI took the screen, SwitchPanicSave.c draws the panic itself (and saves the record): taking XNU's
         * console back has gc_enable kalloc its text buffers with interrupts off, which zalloc_ext's MACH_ASSERT turns
         * into a nested panic that leaves console output disabled. Before the GUI, XNU's console is still on and shows
         * the panic. In graphics mode there is no console: SwitchPanicSave.c always draws it. */
        const int take_screen = !native_text_console || graphics_owns_display;
        if (first && !switch_panic_begin(format, args, caller, take_screen) && native_text_console &&
            graphics_owns_display) {
            graphics_owns_display = 0;
            PE_initialize_console(NULL, kPEAcquireScreen);
        }
        return; /* Let XNU emit its normal panic and backtrace. */
    }
    switch_diag_hud_panic(format, args, caller);
}
