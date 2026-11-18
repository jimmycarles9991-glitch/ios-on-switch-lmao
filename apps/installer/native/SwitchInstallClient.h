#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol SwitchInstallerClient <NSObject>
+ (instancetype)installerWithProgressBlock:(nullable id)progress
          releaseTerminationAssertionBlock:(nullable id)releaseAssertion;
- (void)installURL:(NSURL *)url
               identity:(nullable id)identity
        targetingDomain:(NSUInteger)domain
            withOptions:(id)options
    returningResultInfo:(BOOL)returnInfo
             completion:(void (^)(BOOL, id _Nullable, id _Nullable, NSError *_Nullable))completion;
@end

@protocol SwitchInstallerOptions <NSObject>
+ (instancetype)alloc;
+ (BOOL)instancesRespondToSelector:(SEL)selector;
- (instancetype)initWithLegacyOptionsDictionary:(NSDictionary *)options;
@end

@protocol SwitchInstallerIdentity <NSObject>
+ (instancetype)alloc;
+ (BOOL)instancesRespondToSelector:(SEL)selector;
- (instancetype)initWithBundleID:(NSString *)identifier;
@end

BOOL switch_install_with_client(id<SwitchInstallerClient> _Nullable client, NSURL *url, NSString *identifier,
                                Class<SwitchInstallerOptions> _Nullable optionsType,
                                Class<SwitchInstallerIdentity> _Nullable identityType,
                                NSError *_Nullable *_Nullable error);

NS_ASSUME_NONNULL_END
