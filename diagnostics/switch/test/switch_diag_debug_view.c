// SwitchDiag.h as a debug kernel's XNU includes it: every hook is a function, so the debug units are linked and called.
#include "diagnostics/switch/SwitchDiag.h"

#include "diagnostics/switch/test/switch_diag_debug_view.h"

#if defined(switch_diag_user_syscall) || defined(switch_diag_stage) || defined(switch_diag_process_event) ||         \
    defined(switch_diag_console_active) || defined(switch_diag_console_putc) || defined(switch_root_event) ||        \
    defined(switch_root_data_ownership) || defined(switch_root_record_begin) || defined(switch_root_record_stage) || \
    defined(switch_diag_panic) || defined(switch_diag_init) || defined(switch_root_boot_prepare)
#error "a debug kernel's diagnostic hooks are functions"
#endif

_Static_assert(__builtin_types_compatible_p(__typeof__(switch_diag_user_syscall),
                                            void(unsigned, int, int, uint64_t, uint64_t, int)),
               "the per-syscall observer");
_Static_assert(__builtin_types_compatible_p(__typeof__(switch_diag_stage), void(const char *)), "the stage line");
_Static_assert(__builtin_types_compatible_p(__typeof__(switch_root_event), void(unsigned, int, uint64_t)),
               "the root record");

int switch_diag_debug_view_hooks(void)
{
    return 1;
}
