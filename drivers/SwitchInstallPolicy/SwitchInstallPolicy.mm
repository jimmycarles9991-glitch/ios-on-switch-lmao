// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "SwitchInstallPolicy.h"
#import "CapabilityPolicy.h"
#import "BundleDiscovery.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <os/log.h>
#include <sys/stat.h>
#include <unistd.h>

@protocol SwitchInstallObject <NSObject>
- (id)options;
- (id)installOptions;
- (BOOL)isSystemAppInstall;
- (BOOL)isPlaceholderInstall;
- (NSURL *)stagingRoot;
- (NSURL *)bundleURL;
- (NSURL *)executableURL;
- (id)bundle;
- (id)parentBundle;
- (NSString *)identifier;
- (NSDictionary *)infoPlistSubset;
- (void)setCodeSigningInfo:(id)info;
- (void)setPluginKitBundles:(NSArray *)bundles;
- (void)setExtensionKitBundles:(NSArray *)bundles;
- (NSArray *)extensionKitBundlesPerformingPlatformValidation:(unsigned char)flags withError:(NSError **)error;
- (id)initWithDictionaryRepresentation:(NSDictionary *)dictionary;
@end

enum SwitchHookSlot {
    SwitchHookInstall,
    SwitchHookInstallBundle,
    SwitchHookBundleInfo,
    SwitchHookValidateBundle,
    SwitchHookInstallEmbeddedProfiles,
    SwitchHookInstallProfilesFromUserOptions,
    SwitchHookInstallProfilesAtStagingRoot,
    SwitchHookValidateAppID,
    SwitchHookBuiltInAppUpgrade,
    SwitchHookAppManagementDomain,
    SwitchHookAppClip,
    SwitchHookValidateAppMetadata,
    SwitchHookValidateExtensions,
    SwitchHookDeviceCapabilities,
    SwitchHookDeviceFamily,
    SwitchHookOSVersion,
    SwitchHookThinning,
    SwitchHookExtensionMetadata,
    SwitchHookExtensionEntitlements,
    SwitchHookExtensionNoDot,
    SwitchHookThirdPartyExtensionPoint,
    SwitchHookPluginKitBundles,
    SwitchHookExtensionKitBundles,
    SwitchHookEligibility,
    SwitchHookCount,
};

struct SwitchPolicyHook {
    SwitchHookSlot slot;
    const char *className;
    const char *selectorName;
    IMP replacement;
    const char *types;
};

static __thread void *switch_installer;
static Ivar switch_verification_state;
static IMP switch_originals[SwitchHookCount];
static Class switch_signing_info_class;
static Class switch_plugin_kit_class;
static Class switch_extension_kit_class;
static const void *switch_signing_info_key = &switch_signing_info_key;
static const void *switch_image_info_key = &switch_image_info_key;
static const void *switch_bundle_info_key = &switch_bundle_info_key;

static Class switch_class(const char *name)
{
    NSString *className = @(name);
#if SWITCH_INSTALL_POLICY_TEST
    className = [@"SwitchFixture" stringByAppendingString:className];
#endif
    return NSClassFromString(className);
}

static NSError *switch_failure(NSString *reason)
{
    return [NSError errorWithDomain:@"com.brunostjohn.switch.install-policy"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : reason}];
}

static BOOL switch_fail(NSError **error, NSString *reason)
{
    if (error)
        *error = switch_failure(reason);
    return NO;
}

static BOOL switch_inside(NSURL *url, NSURL *directory)
{
    if (!url.isFileURL || !directory.isFileURL)
        return NO;
    NSString *path = url.URLByResolvingSymlinksInPath.path;
    NSString *root = directory.URLByResolvingSymlinksInPath.path;
    return root.length && ([path isEqualToString:root] || [path hasPrefix:[root stringByAppendingString:@"/"]]);
}

static BOOL switch_candidate(id object)
{
    id<SwitchInstallObject> installer = (__bridge id)switch_installer;
    return installer && [object respondsToSelector:@selector(bundleURL)] &&
           switch_inside([object bundleURL], [installer stagingRoot]);
}

static BOOL switch_scoped(void)
{
    return switch_installer != NULL;
}

static BOOL switch_install(id<SwitchInstallObject> self, SEL selector, NSError **error)
{
    auto original = (BOOL (*)(id, SEL, NSError **))switch_originals[SwitchHookInstall];
    id<SwitchInstallObject> options = [self options];
    void *previous = switch_installer;
    switch_installer = [options isSystemAppInstall] ? NULL : (__bridge void *)self;
    @try {
        return original(self, selector, error);
    } @finally {
        switch_installer = previous;
    }
}

static BOOL switch_mark(NSURL *url, NSError **error)
{
    int directory = open(url.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return switch_fail(error, @"Could not open the imported app bundle");
    int result = unlinkat(directory, ".SwitchUnsigned", 0);
    int failure = result && errno != ENOENT ? errno : 0;
    int marker = failure ? -1 : openat(directory, ".SwitchUnsigned", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                      0600);
    if (marker < 0 && !failure)
        failure = errno;
    if (marker >= 0) {
        if (fchmod(marker, 0600))
            failure = errno;
        if (close(marker) && !failure)
            failure = errno;
    }
    close(directory);
    if (failure) {
        if (error)
            *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:failure userInfo:nil];
        return NO;
    }
    return YES;
}

static BOOL switch_install_bundle(id<SwitchInstallObject> self, SEL selector, NSError **error)
{
    auto original = (BOOL (*)(id, SEL, NSError **))switch_originals[SwitchHookInstallBundle];
    id<SwitchInstallObject> bundle = [self bundle];
    NSURL *source = [bundle bundleURL];
    if (!original(self, selector, error))
        return NO;
    if (!switch_scoped() || !objc_getAssociatedObject(bundle, switch_signing_info_key) || [self isPlaceholderInstall])
        return YES;
    id installer = (__bridge id)switch_installer;
    return switch_publish_capabilities(source, [bundle bundleURL],
        objc_getAssociatedObject(installer, switch_image_info_key),
        objc_getAssociatedObject(installer, switch_bundle_info_key), error) && switch_mark([bundle bundleURL], error);
}

static void switch_record_capabilities(const void *key, NSURL *url, NSDictionary *capabilities)
{
    if (!url.isFileURL)
        return;
    id installer = (__bridge id)switch_installer;
    NSMutableDictionary *records = objc_getAssociatedObject(installer, key);
    if (!records) {
        records = [NSMutableDictionary dictionary];
        objc_setAssociatedObject(installer, key, records, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    }
    records[url.URLByResolvingSymlinksInPath.path] = capabilities;
}

static id switch_signing_info(id<SwitchInstallObject> bundle, NSError **error)
{
    id info = objc_getAssociatedObject(bundle, switch_signing_info_key);
    if (info)
        return info;
    NSString *identifier = [bundle identifier];
    if (![identifier isKindOfClass:NSString.class] || !identifier.length) {
        switch_fail(error, @"The imported bundle has no identifier");
        return nil;
    }
    NSURL *executable = [bundle executableURL];
    NSDictionary *capabilities = switch_filter_capabilities(switch_declared_capabilities(executable), identifier, nil);
    // The legacy dictionary lets MICodeSigningInfo derive its ad-hoc/no-profile enums.
    NSMutableDictionary *dictionary = [@{
        @"AdHocSigned" : @YES,
        @"ValidatedByProfile" : @NO,
        @"CodeInfoIdentifier" : identifier,
        @"com.apple.MobileContainerManager.Entitlements" : capabilities,
    } mutableCopy];
    if (capabilities[@"com.apple.developer.team-identifier"])
        dictionary[@"TeamIdentifier"] = capabilities[@"com.apple.developer.team-identifier"];
    info = [(id<SwitchInstallObject>)[switch_signing_info_class alloc] initWithDictionaryRepresentation:dictionary];
    if (!info) {
        switch_fail(error, @"Could not create ordinary app signing metadata");
        return nil;
    }
    [bundle setCodeSigningInfo:info];
    uint64_t verified = 2;
    memcpy((char *)(__bridge void *)bundle + ivar_getOffset(switch_verification_state), &verified, sizeof(verified));
    objc_setAssociatedObject(bundle, switch_signing_info_key, info, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    switch_record_capabilities(switch_image_info_key, executable, capabilities);
    switch_record_capabilities(switch_bundle_info_key, [bundle bundleURL], capabilities);
    if (error)
        *error = nil;
    return info;
}

static id switch_bundle_info(id self, SEL selector, BOOL resources, BOOL online, BOOL cached, BOOL trust, BOOL profile,
                             NSError **error)
{
    auto original = (id (*)(id, SEL, BOOL, BOOL, BOOL, BOOL, BOOL, NSError **))switch_originals[SwitchHookBundleInfo];
    id info = original(self, selector, resources, online, cached, trust, profile, error);
    return !info && switch_candidate(self) ? switch_signing_info(self, error) : info;
}

static id switch_validate_bundle(id self, SEL selector, id bundle, BOOL resources, BOOL online, BOOL trust, BOOL free,
                                 BOOL profile, NSError **error)
{
    auto original =
        (id (*)(id, SEL, id, BOOL, BOOL, BOOL, BOOL, BOOL, NSError **))switch_originals[SwitchHookValidateBundle];
    id info = original(self, selector, bundle, resources, online, trust, free, profile, error);
    return !info && switch_candidate(bundle) ? switch_signing_info(bundle, error) : info;
}

template <SwitchHookSlot Slot>
static BOOL switch_allow0(id self, SEL selector, NSError **error)
{
    if (!switch_scoped())
        return ((BOOL (*)(id, SEL, NSError **))switch_originals[Slot])(self, selector, error);
    if (error)
        *error = nil;
    return YES;
}

template <SwitchHookSlot Slot>
static BOOL switch_allow1(id self, SEL selector, id value, NSError **error)
{
    if (!switch_scoped())
        return ((BOOL (*)(id, SEL, id, NSError **))switch_originals[Slot])(self, selector, value, error);
    if (error)
        *error = nil;
    return YES;
}

static BOOL switch_allow_appclip(id self, SEL selector, NSDictionary *entitlements, BOOL *clip, NSError **error)
{
    if (!switch_scoped())
        return ((BOOL (*)(id, SEL, id, BOOL *, NSError **))switch_originals[SwitchHookAppClip])(
            self, selector, entitlements, clip, error);
    id value = entitlements[@"com.apple.developer.on-demand-install-capable"];
    if (clip)
        *clip = [value isKindOfClass:NSNumber.class] && [value boolValue];
    if (error)
        *error = nil;
    return YES;
}

static BOOL switch_allow_third_party_extension(id self, SEL selector, BOOL *allowed, NSError **error)
{
    if (!switch_scoped())
        return ((BOOL (*)(id, SEL, BOOL *, NSError **))switch_originals[SwitchHookThirdPartyExtensionPoint])(
            self, selector, allowed, error);
    if (allowed)
        *allowed = YES;
    if (error)
        *error = nil;
    return YES;
}

template <SwitchHookSlot Slot, BOOL ExtensionKit>
static id switch_extension_bundles(id<SwitchInstallObject> self, SEL selector, unsigned char flags, NSError **error)
{
    if (switch_scoped() && [self parentBundle])
        return @[];
    auto original = (id (*)(id, SEL, unsigned char, NSError **))switch_originals[Slot];
    NSArray *known = original(self, selector, flags, error);
    if (!switch_scoped() || !known)
        return known;
    Class targetClass = ExtensionKit ? switch_extension_kit_class : switch_plugin_kit_class;
    NSArray *bundles = switch_discover_extensions(self, targetClass, ExtensionKit, known, error);
    if (bundles) {
        if (ExtensionKit)
            [self setExtensionKitBundles:bundles];
        else
            [self setPluginKitBundles:bundles];
    }
    return bundles;
}

#if SWITCH_INSTALL_POLICY_IOS27
static BOOL switch_allow_eligibility(id self, SEL selector, id info, id distributor, BOOL web, NSError **error)
{
    if (!switch_scoped())
        return ((BOOL (*)(id, SEL, id, id, BOOL, NSError **))switch_originals[SwitchHookEligibility])(
            self, selector, info, distributor, web, error);
    if (error)
        *error = nil;
    return YES;
}
#endif

#define SWITCH_HOOK(slot, className, selector, replacement, types) \
    {slot, className, selector, (IMP)replacement, types}

static const SwitchPolicyHook switch_hooks[] = {
    SWITCH_HOOK(SwitchHookInstall, "MIInstaller", "performInstallationWithError:", switch_install, "B@:^@"),
    SWITCH_HOOK(SwitchHookInstallBundle, "MIInstallableBundle", "performInstallationWithError:", switch_install_bundle,
                "B@:^@"),
    SWITCH_HOOK(SwitchHookBundleInfo, "MIExecutableBundle",
                "codeSigningInfoByValidatingResources:performingOnlineAuthorization:"
                "ignoringCachedSigningInfo:checkingTrustCacheIfApplicable:skippingProfileIDValidation:error:",
                switch_bundle_info, "@@:BBBBB^@"),
    SWITCH_HOOK(SwitchHookValidateBundle, "MIInstallableBundle",
                "_validateBundle:validatingResources:performingOnlineAuthorization:"
                "checkingTrustCacheIfApplicable:allowingFreeProfileValidation:skippingProfileIDValidation:error:",
                switch_validate_bundle, "@@:@BBBBB^@"),
    SWITCH_HOOK(SwitchHookInstallEmbeddedProfiles, "MIInstallableBundle", "_installEmbeddedProfilesWithError:",
                switch_allow0<SwitchHookInstallEmbeddedProfiles>, "B@:^@"),
    SWITCH_HOOK(SwitchHookInstallProfilesFromUserOptions, "MIInstaller",
                "_installProvisioningProfilesFromUserOptionsWithError:",
                switch_allow0<SwitchHookInstallProfilesFromUserOptions>, "B@:^@"),
    SWITCH_HOOK(SwitchHookInstallProfilesAtStagingRoot, "MIInstaller",
                "_installProvisioningProfilesAtStagingRootWithError:",
                switch_allow0<SwitchHookInstallProfilesAtStagingRoot>, "B@:^@"),
    SWITCH_HOOK(SwitchHookValidateAppID, "MIInstallableBundle",
                "_validateApplicationIdentifierForNewBundleSigningInfo:error:",
                switch_allow1<SwitchHookValidateAppID>, "B@:@^@"),
    SWITCH_HOOK(SwitchHookBuiltInAppUpgrade, "MIInstallableBundle",
                "_performBuiltInAppUpgradeValidationWithSigningInfo:error:",
                switch_allow1<SwitchHookBuiltInAppUpgrade>, "B@:@^@"),
    SWITCH_HOOK(SwitchHookAppManagementDomain, "MIInstallableBundle",
                "_validateAppManagementDomainForSigningInfo:error:",
                switch_allow1<SwitchHookAppManagementDomain>, "B@:@^@"),
    SWITCH_HOOK(SwitchHookAppClip, "MIInstallableBundle",
                "_performAppClipSpecificValidationForEntitlements:isAppClip:withError:", switch_allow_appclip,
                "B@:@^B^@"),
    SWITCH_HOOK(SwitchHookValidateAppMetadata, "MIBundle", "validateAppMetadataWithError:",
                switch_allow0<SwitchHookValidateAppMetadata>, "B@:^@"),
    SWITCH_HOOK(SwitchHookValidateExtensions, "MIBundle", "_validateExtensions:error:",
                switch_allow1<SwitchHookValidateExtensions>, "B@:@^@"),
    SWITCH_HOOK(SwitchHookDeviceCapabilities, "MIBundle", "isApplicableToCurrentDeviceCapabilitiesWithError:",
                switch_allow0<SwitchHookDeviceCapabilities>, "B@:^@"),
    SWITCH_HOOK(SwitchHookDeviceFamily, "MIBundle", "isApplicableToCurrentDeviceFamilyWithError:",
                switch_allow0<SwitchHookDeviceFamily>, "B@:^@"),
    SWITCH_HOOK(SwitchHookOSVersion, "MIBundle", "isApplicableToCurrentOSVersionWithError:",
                switch_allow0<SwitchHookOSVersion>, "B@:^@"),
    SWITCH_HOOK(SwitchHookThinning, "MIBundle", "thinningMatchesCurrentDeviceWithError:",
                switch_allow0<SwitchHookThinning>, "B@:^@"),
    SWITCH_HOOK(SwitchHookExtensionMetadata, "MIAppExtensionBundle", "validateBundleMetadataWithError:",
                switch_allow0<SwitchHookExtensionMetadata>, "B@:^@"),
    SWITCH_HOOK(SwitchHookExtensionEntitlements, "MIAppExtensionBundle", "validateHasCorrespondingEntitlements:error:",
                switch_allow1<SwitchHookExtensionEntitlements>, "B@:@^@"),
    SWITCH_HOOK(SwitchHookExtensionNoDot, "MIAppExtensionBundle", "validateHasNoDotInBundleIDSuffix:",
                switch_allow0<SwitchHookExtensionNoDot>, "B@:^@"),
    SWITCH_HOOK(SwitchHookThirdPartyExtensionPoint, "MIExtensionKitBundle",
                "getMayTargetThirdPartyExtensionPoint:withError:", switch_allow_third_party_extension, "B@:^B^@"),
    SWITCH_HOOK(SwitchHookPluginKitBundles, "MIBundle", "pluginKitBundlesPerformingPlatformValidation:withError:",
                (switch_extension_bundles<SwitchHookPluginKitBundles, NO>), "@@:C^@"),
    SWITCH_HOOK(SwitchHookExtensionKitBundles, "MIBundle",
                "extensionKitBundlesPerformingPlatformValidation:withError:",
                (switch_extension_bundles<SwitchHookExtensionKitBundles, YES>), "@@:C^@"),
#if SWITCH_INSTALL_POLICY_IOS27
    SWITCH_HOOK(SwitchHookEligibility, "MIInstallableBundle",
                "_checkEligibilityForAppWithSigningInfo:distributorInfo:isWebDistribution:withError:",
                switch_allow_eligibility, "B@:@@B^@"),
#else
    SWITCH_HOOK(SwitchHookEligibility, "MIInstallableBundle", "_checkEligibilityForEntitlements:withError:",
                switch_allow1<SwitchHookEligibility>, "B@:@^@"),
#endif
};

BOOL switch_install_policy_activate(NSError **error)
{
    static BOOL active;
    if (active)
        return YES;
    Class bundle = switch_class("MIExecutableBundle");
    switch_verification_state = class_getInstanceVariable(bundle, "_codeSignatureVerificationState");
    if (!switch_verification_state || strcmp(ivar_getTypeEncoding(switch_verification_state), @encode(uint64_t)))
        return switch_fail(error, @"The installed bundle verification state has an unknown ABI");
    switch_signing_info_class = switch_class("MICodeSigningInfo");
    switch_plugin_kit_class = switch_class("MIPluginKitBundle");
    switch_extension_kit_class = switch_class("MIExtensionKitBundle");
    IMP resolved[SwitchHookCount] = {};
    for (const auto &hook : switch_hooks) {
        Method method = class_getInstanceMethod(switch_class(hook.className), sel_registerName(hook.selectorName));
        NSMethodSignature *expected = [NSMethodSignature signatureWithObjCTypes:hook.types];
        BOOL matches = method && method_getNumberOfArguments(method) == expected.numberOfArguments;
        for (NSUInteger index = 0; matches && index <= expected.numberOfArguments; ++index) {
            char *actual = index ? method_copyArgumentType(method, (unsigned)index - 1) : method_copyReturnType(method);
            const char *wanted = index ? [expected getArgumentTypeAtIndex:index - 1] : expected.methodReturnType;
#if SWITCH_INSTALL_POLICY_TEST
            if (!strcmp(actual, @encode(BOOL)) && !strcmp(wanted, "B"))
                wanted = actual;
            if (!strcmp(actual, @encode(BOOL *)) && !strcmp(wanted, "^B"))
                wanted = actual;
#endif
            matches = !strcmp(actual, wanted);
            free(actual);
        }
        if (!matches)
            return switch_fail(error, [NSString stringWithFormat:@"Unknown install-policy ABI: %s %s", hook.className,
                                                               hook.selectorName]);
        resolved[hook.slot] = method_getImplementation(method);
    }
    for (const auto &hook : switch_hooks) {
        switch_originals[hook.slot] = resolved[hook.slot];
        Class cls = switch_class(hook.className);
        SEL selector = sel_registerName(hook.selectorName);
        Method method = class_getInstanceMethod(cls, selector);
        class_replaceMethod(cls, selector, hook.replacement, method_getTypeEncoding(method));
    }
    active = YES;
    return YES;
}

#if !SWITCH_INSTALL_POLICY_TEST
__attribute__((constructor)) static void switch_install_policy_start(void)
{
    @autoreleasepool {
        NSError *error = nil;
        if (!dlopen("/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary",
                    RTLD_NOW) ||
            !switch_install_policy_activate(&error)) {
            os_log_error(OS_LOG_DEFAULT, "SwitchInstallPolicy: %{public}@", error ?: @"InstalledContentLibrary unavailable");
            _exit(EXIT_FAILURE);
        }
    }
}
#endif
