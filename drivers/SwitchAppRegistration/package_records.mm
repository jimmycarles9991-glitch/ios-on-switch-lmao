// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "package_records.h"

#include <dlfcn.h>

extern "C" OSStatus SecStaticCodeCreateWithPathAndAttributes(CFURLRef, uint32_t, CFDictionaryRef, CFTypeRef *);
extern "C" OSStatus SecCodeCopySigningInformation(CFTypeRef, uint32_t, CFDictionaryRef *);
extern "C" const CFStringRef kSecCodeInfoEntitlementsDict;
extern "C" const CFStringRef kSecCodeInfoFlags;
extern "C" const CFStringRef kSecCodeInfoIdentifier;
extern "C" const CFStringRef kSecCodeInfoTeamIdentifier;

static constexpr uint32_t switch_signing_information = (1u << 1) | (1u << 2);
static constexpr uint32_t switch_ad_hoc_code = 1u << 1;

@protocol SwitchMCMContainer <NSObject>
+ (instancetype)containerWithIdentifier:(NSString *)identifier
                      createIfNecessary:(BOOL)create
                                existed:(BOOL *)existed
                                  error:(NSError **)error;
- (NSURL *)url;
@end

static NSError *switch_package_error(NSString *description)
{
    return [NSError errorWithDomain:@"com.brunostjohn.switch.appregd"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : description}];
}

static NSDictionary *switch_package_signing_info(NSString *executable, NSError **error)
{
    CFTypeRef image = NULL;
    OSStatus status = SecStaticCodeCreateWithPathAndAttributes(
        (__bridge CFURLRef)[NSURL fileURLWithPath:executable isDirectory:NO], 0, NULL, &image);
    CFDictionaryRef info = NULL;
    if (!status && image)
        status = SecCodeCopySigningInformation(image, switch_signing_information, &info);
    if (image)
        CFRelease(image);
    if (status || !info) {
        if (info)
            CFRelease(info);
        if (error)
            *error = status ? [NSError errorWithDomain:NSOSStatusErrorDomain code:status userInfo:nil]
                            : switch_package_error(@"Security returned no signing information");
        return nil;
    }
    return CFBridgingRelease(info);
}

static NSString *switch_package_container(NSString *containerClass, NSString *identifier, NSError **error)
{
    Class<SwitchMCMContainer> factory = NSClassFromString(containerClass);
    id<SwitchMCMContainer> container = [factory containerWithIdentifier:identifier
                                                      createIfNecessary:YES
                                                                existed:NULL
                                                                  error:error];
    NSString *path = container.url.path;
    if (!path.length && error && !*error)
        *error = switch_package_error([NSString stringWithFormat:@"%@ has no path for %@", containerClass, identifier]);
    return path;
}

static BOOL switch_package_add_groups(NSMutableDictionary *record, NSDictionary *entitlements, NSError **error)
{
    NSMutableDictionary<NSString *, NSString *> *paths = [NSMutableDictionary new];

    const struct {
        NSString *entitlement;
        NSString *containerClass;
        NSString *flag;
    } groups[] = {
        {@"com.apple.security.application-groups", @"MCMSharedDataContainer", @"HasAppGroupContainers"},
        {@"com.apple.security.system-groups", @"MCMSystemDataContainer", @"HasSystemGroupContainers"},
    };

    for (const auto &group : groups) {
        id identifiers = entitlements[group.entitlement];
        if (!identifiers)
            continue;
        if (![identifiers isKindOfClass:NSArray.class]) {
            if (error)
                *error = switch_package_error(@"Invalid signed group container identifiers");
            return NO;
        }
        for (NSString *identifier in identifiers) {
            if (![identifier isKindOfClass:NSString.class] || !identifier.length) {
                if (error)
                    *error = switch_package_error(@"Invalid signed group container identifier");
                return NO;
            }
            NSString *path = switch_package_container(group.containerClass, identifier, error);
            if (!path.length)
                return NO;
            paths[identifier] = path;
            record[group.flag] = @YES;
        }
    }
    if (paths.count)
        record[@"GroupContainers"] = paths;
    return YES;
}

static NSDictionary *switch_package_record(NSDictionary *bundle, NSString *owner, NSError **error)
{
    NSString *path = bundle[@"Path"];
    NSString *identifier = bundle[@"CFBundleIdentifier"];
    NSDictionary *signature =
        switch_package_signing_info([path stringByAppendingPathComponent:bundle[@"CFBundleExecutable"]], error);
    if (!signature)
        return nil;
    NSDictionary *entitlements = signature[(__bridge NSString *)kSecCodeInfoEntitlementsDict];
    if (![entitlements isKindOfClass:NSDictionary.class])
        entitlements = @{};
    id required = entitlements[@"com.apple.private.security.container-required"];
    id noContainer = entitlements[@"com.apple.private.security.no-container"];
    const BOOL containerized = !([noContainer isKindOfClass:NSNumber.class] && [noContainer boolValue]) &&
                               !([required isKindOfClass:NSNumber.class] && ![required boolValue]);
    NSString *containerID = [required isKindOfClass:NSString.class] && [required length] ? required : identifier;
    NSString *container = switch_package_container(owner ? @"MCMPluginKitPluginDataContainer" : @"MCMAppDataContainer",
                                                   containerID, error);
    if (!container.length)
        return nil;
    NSString *home = containerized ? container : @"/var/mobile";
    NSString *temporary = containerized ? [container stringByAppendingPathComponent:@"tmp"] : @"/var/tmp";
    NSMutableDictionary *record = [@{
        @"ApplicationType" : owner ? @"PluginKitPlugin" : @"System",
        @"CFBundleIdentifier" : identifier,
        @"CodeInfoIdentifier" : signature[(__bridge NSString *)kSecCodeInfoIdentifier] ?: identifier,
        @"CompatibilityState" : @0,
        @"Container" : container,
        @"Entitlements" : entitlements,
        @"EnvironmentVariables" : @{@"CFFIXED_USER_HOME" : home, @"HOME" : home, @"TMPDIR" : temporary},
        @"IsContainerized" : @(containerized),
        @"Path" : path,
    } mutableCopy];
    id team = signature[(__bridge NSString *)kSecCodeInfoTeamIdentifier]
                  ?: entitlements[@"com.apple.developer.team-identifier"];
    if ([team isKindOfClass:NSString.class])
        record[@"TeamIdentifier"] = team;
    if (owner)
        record[@"PluginOwnerBundleID"] = owner;
    else {
        id flags = signature[(__bridge NSString *)kSecCodeInfoFlags];
        record[@"IsAdHocSigned"] =
            @([flags isKindOfClass:NSNumber.class] && ([flags unsignedIntValue] & switch_ad_hoc_code));
        record[@"IsDeletable"] = @NO;
        record[@"LSInstallType"] = @1;
    }
    return switch_package_add_groups(record, entitlements, error) ? record : nil;
}

NSArray<NSDictionary *> *switch_package_install_info(NSArray<NSDictionary *> *bundles, NSError **error)
{
    static void *const framework =
        dlopen("/System/Library/PrivateFrameworks/MobileContainerManager.framework/MobileContainerManager", RTLD_NOW);
    if (!framework) {
        if (error)
            *error = switch_package_error(@"MobileContainerManager is unavailable");
        return nil;
    }
    NSMutableArray *records = [NSMutableArray arrayWithCapacity:bundles.count];
    NSString *owner = nil;
    for (NSDictionary *bundle in bundles) {
        NSDictionary *record = switch_package_record(bundle, owner, error);
        if (!record)
            return nil;
        [records addObject:record];
        if (!owner)
            owner = bundle[@"CFBundleIdentifier"];
    }
    return records;
}
