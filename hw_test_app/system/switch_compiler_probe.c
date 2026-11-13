#include "switch_compiler_probe.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

#include "../../drivers/TegraGM20BMetal/compiler/SwitchShaderCompilerJetsam.h"

/* xnu bsd/sys/kern_memorystatus.h: not in the SDK, the same on xnu-10063 and xnu-12377. */
#define SWITCH_MEMORYSTATUS_CMD_GET_PRIORITY_LIST       1
#define SWITCH_MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES 8
#define SWITCH_MEMORYSTATUS_MEMLIMIT_ATTR_FATAL         0x1u

struct switch_memorystatus_priority_entry {
    int32_t pid;
    int32_t priority;
    uint64_t user_data;
    int32_t limit;
    uint32_t state;
};

struct switch_memorystatus_memlimit_properties {
    int32_t memlimit_active;
    uint32_t memlimit_active_attr;
    int32_t memlimit_inactive;
    uint32_t memlimit_inactive_attr;
};

extern int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);

static void read_line(struct switch_compiler_probe *out)
{
    char line[96] = {0};
    size_t size = sizeof(line) - 1;
    if (sysctlbyname("kern.switch_shader_compiler", line, &size, NULL, 0)) {
        out->line_error = errno;
        return;
    }
    unsigned generation = 0, losses = 0;
    int pid = 0;
    if (sscanf(line, "pid=%d generation=%u losses=%u", &pid, &generation, &losses) != 3) {
        out->line_error = EBADMSG;
        return;
    }
    out->pid = pid;
    out->generation = generation;
    out->losses = losses;
}

static void read_jetsam(struct switch_compiler_probe *out)
{
    struct switch_memorystatus_priority_entry entry = {0};
    if (memorystatus_control(SWITCH_MEMORYSTATUS_CMD_GET_PRIORITY_LIST, out->pid, 0, &entry, sizeof(entry)) < 0) {
        out->priority_error = errno;
    } else {
        out->band = entry.priority;
        out->limit_mb = entry.limit;
        out->state = entry.state;
    }
    struct switch_memorystatus_memlimit_properties limits = {0};
    if (memorystatus_control(SWITCH_MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, out->pid, 0, &limits, sizeof(limits)) <
        0) {
        out->limits_error = errno;
        return;
    }
    out->active_mb = limits.memlimit_active;
    out->inactive_mb = limits.memlimit_inactive;
    out->active_fatal = (limits.memlimit_active_attr & SWITCH_MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) != 0;
    out->inactive_fatal = (limits.memlimit_inactive_attr & SWITCH_MEMORYSTATUS_MEMLIMIT_ATTR_FATAL) != 0;
}

void switch_compiler_probe(struct switch_compiler_probe *out)
{
    memset(out, 0, sizeof(*out));
    out->wanted_band = SWITCH_SHADER_COMPILER_JETSAM_PRIORITY;
    out->wanted_active_mb = SWITCH_SHADER_COMPILER_ACTIVE_LIMIT_MB;
    out->wanted_inactive_mb = SWITCH_SHADER_COMPILER_INACTIVE_LIMIT_MB;
    read_line(out);
    if (out->pid > 0)
        read_jetsam(out);
}
