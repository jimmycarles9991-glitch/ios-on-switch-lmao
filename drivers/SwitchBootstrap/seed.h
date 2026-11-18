// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

int switch_bootstrap_prepare(NSString *root, NSString *base, NSString *archive, NSString *expectedHash);
int switch_bootstrap_restore_seed(NSString *root, NSString *base);
