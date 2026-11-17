// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

NSDictionary *switch_declared_capabilities(NSURL *image);
NSDictionary *switch_filter_capabilities(id declared, NSString *identity, NSDictionary *inherited);
BOOL switch_publish_capabilities(NSURL *sourceRoot, NSURL *destinationRoot, NSDictionary *images,
                                 NSDictionary *bundles, NSError **error);
