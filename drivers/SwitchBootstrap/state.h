// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

NSString *switch_bootstrap_digest(NSString *path);
BOOL switch_bootstrap_metadata(NSString *path, BOOL directory);
int switch_bootstrap_readiness(int ready);
int switch_bootstrap_update_command(NSString *path, NSArray<NSString *> *arguments);
