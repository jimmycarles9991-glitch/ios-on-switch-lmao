// SPDX-License-Identifier: GPL-2.0-only
#include <sys/errno.h>
#include <sys/persona.h>
#include <sys/proc.h>
#include <sys/vm.h>
#include <sys/spawn_internal.h>
#include <kern/locks.h>
#include <switch/bootstrap.h>

#if CONFIG_PERSONAS
static struct persona *switch_bootstrap_persona;
LCK_GRP_DECLARE(switch_bootstrap_persona_lck_grp, "switch-bootstrap-persona");
LCK_MTX_DECLARE(switch_bootstrap_persona_lock, &switch_bootstrap_persona_lck_grp);

bool switch_bootstrap_persona_request(const struct _posix_spawn_persona_info *info)
{
    const uint32_t flags = POSIX_SPAWN_PERSONA_FLAGS_OVERRIDE | POSIX_SPAWN_PERSONA_UID | POSIX_SPAWN_PERSONA_GID;
    return info && info->pspi_id == SWITCH_BOOTSTRAP_PERSONA && info->pspi_flags == flags &&
        info->pspi_uid == 0 && info->pspi_gid == 0 && switch_bootstrap_persona_caller(current_proc());
}

int switch_bootstrap_persona_init(void)
{
    int error = 0;
    lck_mtx_lock(&switch_bootstrap_persona_lock);
    struct persona *persona = persona_lookup(SWITCH_BOOTSTRAP_PERSONA);
    if (persona) {
        if (!switch_bootstrap_persona) switch_bootstrap_persona = persona;
        else {
            error = persona == switch_bootstrap_persona ? 0 : EEXIST;
            persona_put(persona);
        }
    } else if (switch_bootstrap_persona) {
        error = ESRCH;
    } else {
        persona = persona_alloc(SWITCH_BOOTSTRAP_PERSONA, "SwitchBootstrap", PERSONA_SYSTEM, NULL, 0, &error);
        if (persona) {
            error = persona_init_begin(persona);
            persona_init_end(persona, error);
            if (error) persona_put(persona);
            else switch_bootstrap_persona = persona;
        }
    }
    lck_mtx_unlock(&switch_bootstrap_persona_lock);
    return error;
}
#endif
