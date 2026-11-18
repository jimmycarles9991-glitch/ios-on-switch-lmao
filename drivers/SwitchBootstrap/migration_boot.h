// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

int switch_bootstrap_prepare_packages(NSString *base, NSString *root);
int switch_bootstrap_prepare_repair(NSString *base, NSString *root);
int switch_bootstrap_validate_root(NSString *root, BOOL prepared = NO);
int switch_bootstrap_start_runtime(NSString *root, NSMutableArray *loaded);
int switch_bootstrap_read_flag(const char *name, int *flag);
int switch_bootstrap_register_apps(BOOL repair);
int switch_migration_boot(NSString *base, NSString *root, NSString *generation);
