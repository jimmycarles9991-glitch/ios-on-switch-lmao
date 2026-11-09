/* The single-CPU diagnostic HUD (SwitchDiagHud.c), debug kernels only: what SwitchDiag.c, built in every kernel config,
 * hands it. In release each call is an empty inline and the HUD is never linked. */
#ifndef SWITCH_DIAG_HUD_H
#define SWITCH_DIAG_HUD_H
#include <stdarg.h>
#include <stdint.h>

struct boot_args;

struct switch_diag_state {
    uint64_t magic;
    uint64_t ready;
    uint64_t stages;
    uint64_t panicked;
    uint64_t framebuffer_va;
    uint64_t caller;
    uint64_t current_el;
    uint64_t sctlr;
    uint64_t tcr;
    uint64_t sampled_esr;
    uint64_t sampled_far;
    uint64_t truncated;
    char stage[104];
    char panic_text[1024];
};
extern struct switch_diag_state switch_diag_record;

#if DEVELOPMENT || DEBUG
void switch_diag_hud_init(const struct boot_args *args, int cpus);
void switch_diag_hud_panic(const char *format, va_list *args, unsigned long caller);
void switch_diag_graphics_takeover(void);
void switch_diag_graphics_release(void);
#else
static inline void switch_diag_hud_init(const struct boot_args *args, int cpus)
{
    (void)args;
    (void)cpus;
}

static inline void switch_diag_hud_panic(const char *format, va_list *args, unsigned long caller)
{
    (void)format;
    (void)args;
    (void)caller;
}

static inline void switch_diag_graphics_takeover(void)
{}

static inline void switch_diag_graphics_release(void)
{}
#endif
#endif
