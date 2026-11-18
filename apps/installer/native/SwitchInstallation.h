#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef void (^SwitchInstallationProgress)(NSString *_Nullable status, NSNumber *_Nullable percent);

@interface SwitchInstallation : NSObject
+ (BOOL)isInstalled:(NSString *)identifier;
+ (BOOL)installURL:(NSURL *)url
        identifier:(NSString *)identifier
          progress:(SwitchInstallationProgress)progress
             error:(NSError **)error;
+ (BOOL)openApplication:(NSString *)identifier;
@end

NS_ASSUME_NONNULL_END
