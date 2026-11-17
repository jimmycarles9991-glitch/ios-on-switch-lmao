// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#import <Foundation/Foundation.h>

NSData *switch_serialize_entitlements(NSDictionary *dictionary);
NSDictionary *switch_deserialize_entitlements(NSData *data);
