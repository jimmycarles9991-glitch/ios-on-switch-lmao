// SPDX-License-Identifier: BSD-2-Clause-Patent
// The freeze heartbeat's part of the panic record (SwitchPanicHeartbeat.h).
#include <stddef.h>
#include <stdint.h>
#include "SwitchPanicHeartbeat.h"
#include "SwitchPowerTrail.h"

#if DEVELOPMENT || DEBUG
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern int snprintf(char *, size_t, const char *, ...);

int switch_panic_heartbeat_watchdog(const char *panic_line)
{
    static const char prefix[] = "SwitchHeartbeat:";
    for (size_t i = 0; i + 1 < sizeof(prefix); ++i) {
        if (panic_line[i] != prefix[i])
            return 0;
    }
    return 1;
}

// The interrupt goes to CPU0 and is written down only once it found the beat late, just before the panic.
const char *switch_panic_heartbeat_story(int watchdog_reset)
{
    static char story[384];
    uint64_t interrupt = 0;
    const uint64_t beat = switch_power_trail_last_beat(&interrupt);
    if (!watchdog_reset || !beat)
        return NULL;
    if (interrupt > beat) {
        const uint64_t late = interrupt - beat;
        snprintf(story, sizeof(story),
                 "The freeze heartbeat's WDT4 reset a hung console: its last beat was %llu.%03llu s after boot; CPU0 "
                 "took its interrupt %llu.%03llu s later, found the beat late and stopped before the panic record was "
                 "written.",
                 beat / 1000000000ULL, beat / 1000000ULL % 1000, late / 1000000000ULL, late / 1000000ULL % 1000);
    } else {
        snprintf(story, sizeof(story),
                 "The freeze heartbeat's WDT4 reset a hung console about 48 s after its last beat at %llu.%03llu s "
                 "after boot. No core ran its interrupt, which goes to CPU0, about 12 s after that beat: CPU0 had "
                 "interrupts masked (class a), or the interrupt never reached it.",
                 beat / 1000000000ULL, beat / 1000000ULL % 1000);
    }
    return story;
}
#endif
