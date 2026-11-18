// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

BOOL switch_bootstrap_core_current(NSString *base, NSString *hash);
int switch_bootstrap_verify_installed_core(NSString *base, NSString *root);
int switch_bootstrap_verify_sileo(NSString *root);
int switch_bootstrap_update_core(NSString *root, NSString *base, NSString *packages, NSString *hash, NSString *helper,
                                 BOOL recovery, BOOL *updated);
