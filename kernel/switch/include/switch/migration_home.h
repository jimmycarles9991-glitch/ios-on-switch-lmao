// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_MIGRATION_HOME_H
#define SWITCH_MIGRATION_HOME_H

#include <stdbool.h>
#include <sys/cdefs.h>

__BEGIN_DECLS
void switch_migration_home_requested(void);
void switch_migration_home_prepare(bool has_payload, bool pending);
__END_DECLS

#endif
