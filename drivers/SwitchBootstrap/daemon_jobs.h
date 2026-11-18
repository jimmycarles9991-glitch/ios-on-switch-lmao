// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

int switch_bootstrap_load_daemons(NSString *root, NSMutableArray<NSString *> *loaded);
void switch_bootstrap_unload_daemons(NSArray<NSString *> *loaded);
