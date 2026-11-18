#import "SwitchInstallation.h"
#import "SwitchInstallClient.h"

#include <TargetConditionals.h>
#include <dlfcn.h>

@protocol SwitchInstallerWorkspace <NSObject>
+ (instancetype)defaultWorkspace;
- (BOOL)installApplication:(NSURL *)url withOptions:(NSDictionary *)options error:(NSError **)error;
- (BOOL)installApplication:(NSURL *)url
               withOptions:(NSDictionary *)options
                     error:(NSError **)error
                usingBlock:(void (^)(NSDictionary *))progress;
- (BOOL)openApplicationWithBundleID:(NSString *)identifier;
@end

@protocol SwitchInstallerApplication <NSObject>
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)isInstalled;
@end

static id<SwitchInstallerWorkspace> switch_workspace(void)
{
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
    static id<SwitchInstallerWorkspace> workspace;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW)) {
            Class<SwitchInstallerWorkspace> type = NSClassFromString(@"LSApplicationWorkspace");
            workspace = [type defaultWorkspace];
        }
    });
    return workspace;
#else
    return nil;
#endif
}

@implementation SwitchInstallation

+ (BOOL)isInstalled:(NSString *)identifier
{
    if (!switch_workspace())
        return NO;
    Class<SwitchInstallerApplication> type = NSClassFromString(@"LSApplicationProxy");
    id<SwitchInstallerApplication> app = [type applicationProxyForIdentifier:identifier];
    return [app respondsToSelector:@selector(isInstalled)] && [app isInstalled];
}

+ (BOOL)installURL:(NSURL *)url
        identifier:(NSString *)identifier
          progress:(SwitchInstallationProgress)progress
             error:(NSError **)error
{
    void (^report)(NSDictionary *) = ^(NSDictionary *update) {
        if (![update isKindOfClass:NSDictionary.class])
            return;
        NSString *status = update[@"Status"];
        NSNumber *percent = update[@"PercentComplete"];
        progress([status isKindOfClass:NSString.class] ? status : nil,
                 [percent isKindOfClass:NSNumber.class] ? percent : nil);
    };
#if TARGET_OS_IOS && __IPHONE_OS_VERSION_MIN_REQUIRED >= 270000
    static Class<SwitchInstallerClient> clientType;
    static Class<SwitchInstallerOptions> optionsType;
    static Class<SwitchInstallerIdentity> identityType;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW)) {
            clientType = NSClassFromString(@"MIInstallerClient");
            optionsType = NSClassFromString(@"MIInstallOptions");
            identityType = NSClassFromString(@"MIAppIdentity");
        }
    });
    id<SwitchInstallerClient> client = nil;
    if ([clientType respondsToSelector:@selector(installerWithProgressBlock:releaseTerminationAssertionBlock:)])
        client = [clientType installerWithProgressBlock:report releaseTerminationAssertionBlock:nil];
    return switch_install_with_client(client, url, identifier, optionsType, identityType, error);
#else
    (void)identifier;
    id<SwitchInstallerWorkspace> workspace = switch_workspace();
    if (!url.isFileURL || ![workspace respondsToSelector:@selector(installApplication:withOptions:error:)]) {
        if (error)
            *error = [NSError
                errorWithDomain:@"com.brunostjohn.switch.installer"
                           code:1
                       userInfo:@{
                           NSLocalizedDescriptionKey : @"On-device installation is unavailable in this environment."
                       }];
        return NO;
    }
    NSError *failure = nil;
    NSDictionary *options = @{@"ApplicationType" : @"User"};
    BOOL installed = [workspace respondsToSelector:@selector(installApplication:withOptions:error:usingBlock:)]
                         ? [workspace installApplication:url withOptions:options error:&failure usingBlock:report]
                         : [workspace installApplication:url withOptions:options error:&failure];
    if (!installed && error)
        *error = failure
                     ?: [NSError errorWithDomain:@"com.brunostjohn.switch.installer"
                                            code:2
                                        userInfo:@{
                                            NSLocalizedDescriptionKey :
                                                @"iOS rejected the installation without reporting a reason."
                                        }];
    return installed;
#endif
}

+ (BOOL)openApplication:(NSString *)identifier
{
    id<SwitchInstallerWorkspace> workspace = switch_workspace();
    return [workspace respondsToSelector:@selector(openApplicationWithBundleID:)] &&
           [workspace openApplicationWithBundleID:identifier];
}

@end
