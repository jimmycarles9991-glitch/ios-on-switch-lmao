// SPDX-License-Identifier: BSD-2-Clause-Patent
// LaunchServices registration of an app in /Applications as installd performs it for a built-in app, through
// LaunchServices' and InstalledContentLibrary's private classes, which switch_app_registration_load finds at run time.
#pragma once

#import <Foundation/Foundation.h>

// LaunchServices calls one of these once lsd has saved, or failed to save, the operation of that UUID.
@protocol LSOperationSaveObserver <NSObject>
- (void)operationWithUUIDWasSaved:(NSUUID *)uuid;
- (void)operationWithUUID:(NSUUID *)uuid didFailToSaveWithError:(NSError *)error;
@end

BOOL switch_app_registration_load(NSError **error);
BOOL switch_app_installed(NSString *identifier);
BOOL switch_register_builtin_app(NSString *path, NSUUID *uuid, id<LSOperationSaveObserver> observer, NSError **error);
BOOL switch_unregister_builtin_app(NSString *path, NSString *identifier, NSUUID *uuid,
                                   id<LSOperationSaveObserver> observer, NSError **error);
