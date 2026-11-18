// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

int switch_registration_serve(void);
int switch_registration_request(NSArray<NSString *> *arguments);
int switch_uicache_main(int argc, char **argv);
int switch_apply_package_registration(NSString *path, NSString *identifier, NSArray<NSDictionary *> *bundles);
