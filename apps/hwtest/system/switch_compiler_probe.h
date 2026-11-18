/* system.compiler_alive (switch_compiler_probe.c): the system shader compiler as the kernel holds it. Its pid and
 * generation come from the GPU kext's broker (kern.switch_shader_compiler, debug kernels); its jetsam band and limits
 * from memorystatus_control on that pid, which the app's com.apple.private.memorystatus entitlement admits. The wanted
 * values are SwitchShaderCompilerJetsam.h's, the ones its launcher gives it. */
#ifndef SWITCH_COMPILER_PROBE_H
#define SWITCH_COMPILER_PROBE_H

#include <stdint.h>

struct switch_compiler_probe {
    int32_t line_error;     /* errno of the sysctl read, 0 when the line was read */
    int32_t pid;            /* the registered compiler's, 0 for none */
    uint32_t generation;    /* compilers registered this boot */
    uint32_t losses;        /* compilers the broker lost this boot */
    int32_t priority_error; /* errno of MEMORYSTATUS_CMD_GET_PRIORITY_LIST for the pid */
    int32_t band;           /* its effective jetsam priority */
    int32_t limit_mb;       /* the limit in force now */
    uint32_t state;         /* its memorystatus state bits (kern_memorystatus.h kMemorystatus*) */
    int32_t limits_error;   /* errno of MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES for the pid */
    int32_t active_mb, inactive_mb;
    uint8_t active_fatal, inactive_fatal;
    int32_t wanted_band, wanted_active_mb, wanted_inactive_mb;
};

void switch_compiler_probe(struct switch_compiler_probe *out);

#endif
