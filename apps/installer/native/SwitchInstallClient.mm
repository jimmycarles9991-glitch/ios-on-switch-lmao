#import "SwitchInstallClient.h"

static NSError *switch_install_client_error(NSInteger code, NSString *reason)
{
    return [NSError errorWithDomain:@"com.brunostjohn.switch.installer"
                               code:code
                           userInfo:@{NSLocalizedDescriptionKey : reason}];
}

BOOL switch_install_with_client(id<SwitchInstallerClient> client, NSURL *url, NSString *identifier,
                                Class<SwitchInstallerOptions> optionsType, Class<SwitchInstallerIdentity> identityType,
                                NSError **error)
{
    if (!url.isFileURL || !identifier.length ||
        ![client
            respondsToSelector:@selector(
                                   installURL:identity:targetingDomain:withOptions:returningResultInfo:completion:)] ||
        ![optionsType instancesRespondToSelector:@selector(initWithLegacyOptionsDictionary:)] ||
        ![identityType instancesRespondToSelector:@selector(initWithBundleID:)]) {
        if (error)
            *error = switch_install_client_error(1, @"The iOS installation service is unavailable.");
        return NO;
    }
    id options = [[optionsType alloc] initWithLegacyOptionsDictionary:@{
        @"PackageType" : @"Customer",
        @"CFBundleIdentifier" : identifier,
        @"IsUserInitiated" : @YES,
    }];
    id identity = [[identityType alloc] initWithBundleID:identifier];
    if (!options || !identity) {
        if (error)
            *error = switch_install_client_error(1, @"iOS could not create the installation request.");
        return NO;
    }
    constexpr NSUInteger switch_installation_domain_default = 1;
    __block BOOL completed = NO;
    __block BOOL installed = NO;
    __block NSError *failure = nil;
    // MIInstallerClient delivers both the XPC reply and connection errors before this call returns.
    [client installURL:url
                   identity:identity
            targetingDomain:switch_installation_domain_default
                withOptions:options
        returningResultInfo:NO
                 completion:^(BOOL success, id, id, NSError *resultError) {
                     completed = YES;
                     installed = success && !resultError;
                     failure = resultError;
                 }];
    if (!installed && error)
        *error = failure
                     ?: switch_install_client_error(
                            2, completed ? @"iOS rejected the installation without reporting a reason."
                                         : @"The iOS installation service did not complete the request.");
    return installed;
}
