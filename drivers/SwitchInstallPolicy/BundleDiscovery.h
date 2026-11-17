// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

NSArray *switch_discover_extensions(id parent, Class nativeClass, BOOL extensionKit, NSArray *known, NSError **error);
