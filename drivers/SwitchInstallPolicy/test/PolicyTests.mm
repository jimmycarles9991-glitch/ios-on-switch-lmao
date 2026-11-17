// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "../SwitchInstallPolicy.h"
#import "../CapabilityPolicy.h"
#import "../EntitlementDER.h"

#include <assert.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/loader.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <unistd.h>

#define MICodeSigningInfo SwitchFixtureMICodeSigningInfo
#define MIBundle SwitchFixtureMIBundle
#define MIExecutableBundle SwitchFixtureMIExecutableBundle
#define MIAppExtensionBundle SwitchFixtureMIAppExtensionBundle
#define MIInstallableBundle SwitchFixtureMIInstallableBundle
#define MIInstaller SwitchFixtureMIInstaller
#define MIExtensionKitBundle SwitchFixtureMIExtensionKitBundle

void switch_policy_capability_tests(NSURL *root);
void switch_policy_source_tests(NSURL *root);

static NSError *failure(void)
{
    return [NSError errorWithDomain:@"fixture" code:1 userInfo:nil];
}

@interface MICodeSigningInfo : NSObject
@property NSDictionary *dictionary;
- (id)initWithDictionaryRepresentation:(NSDictionary *)dictionary;
@end
@implementation MICodeSigningInfo
- (id)initWithDictionaryRepresentation:(NSDictionary *)dictionary
{
    if ((self = [super init]))
        _dictionary = [dictionary copy];
    return self;
}
@end

@interface SwitchFixtureBundleBase : NSObject
- (BOOL)_validateExtensions:(id)extensions error:(NSError **)error;
@end
@implementation SwitchFixtureBundleBase
- (BOOL)_validateExtensions:(id)extensions error:(NSError **)error { (void)extensions; (void)error; return NO; }
@end

@interface MIBundle : SwitchFixtureBundleBase
@property NSURL *bundleURL;
@property NSString *identifier;
@property NSDictionary *infoPlistSubset;
@property (weak) MIBundle *parentBundle;
@property NSArray *pluginKitBundles;
@property NSArray *extensionKitBundles;
- (NSArray *)pluginKitBundlesPerformingPlatformValidation:(unsigned char)flags withError:(NSError **)error;
- (NSArray *)extensionKitBundlesPerformingPlatformValidation:(unsigned char)flags withError:(NSError **)error;
- (NSURL *)executableURL;
- (BOOL)validateBundleMetadataWithError:(NSError **)error;
- (BOOL)validateAppMetadataWithError:(NSError **)error;
- (BOOL)isApplicableToCurrentDeviceCapabilitiesWithError:(NSError **)error;
- (BOOL)isApplicableToCurrentDeviceFamilyWithError:(NSError **)error;
- (BOOL)isApplicableToCurrentOSVersionWithError:(NSError **)error;
- (BOOL)thinningMatchesCurrentDeviceWithError:(NSError **)error;
@end
@implementation MIBundle
- (NSArray *)pluginKitBundlesPerformingPlatformValidation:(unsigned char)flags withError:(NSError **)error
{ (void)flags; (void)error; return self.pluginKitBundles ?: @[]; }
- (NSArray *)extensionKitBundlesPerformingPlatformValidation:(unsigned char)flags withError:(NSError **)error
{ (void)flags; (void)error; return self.extensionKitBundles ?: @[]; }
- (NSURL *)executableURL { return [self.bundleURL URLByAppendingPathComponent:@"Executable"]; }
- (BOOL)validateBundleMetadataWithError:(NSError **)error
{
    if (!self.identifier.length && error)
        *error = failure();
    return self.identifier.length != 0;
}
- (BOOL)validateAppMetadataWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)isApplicableToCurrentDeviceCapabilitiesWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)isApplicableToCurrentDeviceFamilyWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)isApplicableToCurrentOSVersionWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)thinningMatchesCurrentDeviceWithError:(NSError **)error { (void)error; return NO; }
@end

@interface MIExecutableBundle : MIBundle {
  @public
    uint64_t _codeSignatureVerificationState;
}
@property MICodeSigningInfo *codeSigningInfo;
- (id)codeSigningInfoByValidatingResources:(BOOL)resources performingOnlineAuthorization:(BOOL)online
                   ignoringCachedSigningInfo:(BOOL)cached checkingTrustCacheIfApplicable:(BOOL)trust
                    skippingProfileIDValidation:(BOOL)profile error:(NSError **)error;
@end
@implementation MIExecutableBundle
- (id)codeSigningInfoByValidatingResources:(BOOL)resources performingOnlineAuthorization:(BOOL)online
                   ignoringCachedSigningInfo:(BOOL)cached checkingTrustCacheIfApplicable:(BOOL)trust
                    skippingProfileIDValidation:(BOOL)profile error:(NSError **)error
{
    (void)resources; (void)online; (void)cached; (void)trust; (void)profile;
    if (error)
        *error = failure();
    return nil;
}
@end

@interface MIAppExtensionBundle : MIExecutableBundle
- (BOOL)validateHasCorrespondingEntitlements:(id)entitlements error:(NSError **)error;
- (BOOL)validateHasNoDotInBundleIDSuffix:(NSError **)error;
@end
@implementation MIAppExtensionBundle
- (BOOL)validateBundleMetadataWithError:(NSError **)error
{
    if (error)
        *error = failure();
    return NO;
}
- (BOOL)validateHasCorrespondingEntitlements:(id)entitlements error:(NSError **)error
{
    (void)entitlements;
    if (error)
        *error = failure();
    return NO;
}
- (BOOL)validateHasNoDotInBundleIDSuffix:(NSError **)error
{
    if (error)
        *error = failure();
    return NO;
}
@end

@interface MIExtensionKitBundle : MIAppExtensionBundle
@property BOOL extensionPointReady;
- (BOOL)getMayTargetThirdPartyExtensionPoint:(BOOL *)allowed withError:(NSError **)error;
@end
@implementation MIExtensionKitBundle
- (BOOL)getMayTargetThirdPartyExtensionPoint:(BOOL *)allowed withError:(NSError **)error
{
    (void)error;
    if (allowed) *allowed = NO;
    return YES;
}
- (BOOL)validateBundleMetadataWithError:(NSError **)error
{
    BOOL allowed = NO;
    if (![super validateBundleMetadataWithError:error] ||
        ![self getMayTargetThirdPartyExtensionPoint:&allowed withError:error] || !allowed)
        return NO;
    self.extensionPointReady = YES;
    return YES;
}
@end

@interface SwitchFixtureOptions : NSObject
@property BOOL isSystemAppInstall;
@end
@implementation SwitchFixtureOptions
@end

@interface MIInstallableBundle : NSObject
@property MIExecutableBundle *bundle;
@property NSArray<MIExecutableBundle *> *children;
@property BOOL isPlaceholderInstall;
@property BOOL forceFailure;
@property BOOL appClip;
@property NSUInteger nativeValidationCalls;
@property NSURL *moveDestination;
- (BOOL)performVerificationWithError:(NSError **)error;
- (BOOL)performInstallationWithError:(NSError **)error;
- (BOOL)_installEmbeddedProfilesWithError:(NSError **)error;
- (BOOL)_validateApplicationIdentifierForNewBundleSigningInfo:(id)info error:(NSError **)error;
- (BOOL)_performBuiltInAppUpgradeValidationWithSigningInfo:(id)info error:(NSError **)error;
- (BOOL)_validateAppManagementDomainForSigningInfo:(id)info error:(NSError **)error;
- (BOOL)_performAppClipSpecificValidationForEntitlements:(id)entitlements isAppClip:(BOOL *)clip withError:(NSError **)error;
- (BOOL)_checkEligibilityForEntitlements:(id)info withError:(NSError **)error;
- (BOOL)_checkEligibilityForAppWithSigningInfo:(id)info distributorInfo:(id)distributor
                           isWebDistribution:(BOOL)web withError:(NSError **)error;
- (id)_validateBundle:(id)bundle validatingResources:(BOOL)resources performingOnlineAuthorization:(BOOL)online
    checkingTrustCacheIfApplicable:(BOOL)trust allowingFreeProfileValidation:(BOOL)free
     skippingProfileIDValidation:(BOOL)profile error:(NSError **)error;
@end
@implementation MIInstallableBundle
- (BOOL)performInstallationWithError:(NSError **)error
{
    if (!self.moveDestination)
        return YES;
    NSString *oldMarker = [self.bundle.bundleURL.path stringByAppendingPathComponent:@".SwitchUnsigned"];
    if (![@"" writeToFile:oldMarker atomically:YES encoding:NSUTF8StringEncoding error:error] ||
        chmod(oldMarker.fileSystemRepresentation, 0644))
        return NO;
    if (![[NSFileManager defaultManager] createDirectoryAtURL:self.moveDestination.URLByDeletingLastPathComponent
                                withIntermediateDirectories:YES attributes:nil error:error] ||
        ![[NSFileManager defaultManager] moveItemAtURL:self.bundle.bundleURL toURL:self.moveDestination error:error])
        return NO;
    self.bundle.bundleURL = self.moveDestination;
    return YES;
}
- (BOOL)performVerificationWithError:(NSError **)error
{
    if (self.forceFailure)
        return NO;
    for (MIExecutableBundle *bundle in [@[ self.bundle ] arrayByAddingObjectsFromArray:self.children ?: @[]]) {
        id info = [self _validateBundle:bundle validatingResources:YES performingOnlineAuthorization:YES
             checkingTrustCacheIfApplicable:YES allowingFreeProfileValidation:NO skippingProfileIDValidation:NO error:error];
        if (!info)
            return NO;
        if ([bundle isKindOfClass:MIAppExtensionBundle.class] &&
            ![(MIAppExtensionBundle *)bundle validateBundleMetadataWithError:error])
            return NO;
    }
    BOOL clip = YES;
    if (![self _performAppClipSpecificValidationForEntitlements:
        self.bundle.codeSigningInfo.dictionary[@"com.apple.MobileContainerManager.Entitlements"] isAppClip:&clip withError:error])
        return NO;
    self.appClip = clip;
    return [self _validateApplicationIdentifierForNewBundleSigningInfo:self.bundle.codeSigningInfo error:error] &&
        [self _validateAppManagementDomainForSigningInfo:self.bundle.codeSigningInfo error:error];
}
- (id)_validateBundle:(id)bundle validatingResources:(BOOL)resources performingOnlineAuthorization:(BOOL)online
    checkingTrustCacheIfApplicable:(BOOL)trust allowingFreeProfileValidation:(BOOL)free
     skippingProfileIDValidation:(BOOL)profile error:(NSError **)error
{
    (void)bundle; (void)resources; (void)online; (void)trust; (void)free; (void)profile;
    ++self.nativeValidationCalls;
    if (error)
        *error = failure();
    return nil;
}
- (BOOL)_installEmbeddedProfilesWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)_validateApplicationIdentifierForNewBundleSigningInfo:(id)info error:(NSError **)error
{ (void)info; (void)error; return NO; }
- (BOOL)_performBuiltInAppUpgradeValidationWithSigningInfo:(id)info error:(NSError **)error
{ (void)info; (void)error; return NO; }
- (BOOL)_validateAppManagementDomainForSigningInfo:(id)info error:(NSError **)error
{ (void)info; (void)error; return NO; }
- (BOOL)_performAppClipSpecificValidationForEntitlements:(id)entitlements isAppClip:(BOOL *)clip withError:(NSError **)error
{ (void)entitlements; (void)clip; (void)error; return NO; }
- (BOOL)_checkEligibilityForEntitlements:(id)info withError:(NSError **)error
{ (void)info; (void)error; return NO; }
- (BOOL)_checkEligibilityForAppWithSigningInfo:(id)info distributorInfo:(id)distributor
                           isWebDistribution:(BOOL)web withError:(NSError **)error
{ (void)info; (void)distributor; (void)web; (void)error; return NO; }
@end

@interface MIInstaller : NSObject
@property SwitchFixtureOptions *options;
@property NSURL *stagingRoot;
@property MIInstallableBundle *installable;
@property NSUInteger stages;
@property BOOL throwException;
@property (copy) void (^duringInstallation)(void);
- (BOOL)performInstallationWithError:(NSError **)error;
- (BOOL)_installProvisioningProfilesFromUserOptionsWithError:(NSError **)error;
- (BOOL)_installProvisioningProfilesAtStagingRootWithError:(NSError **)error;
@end
@implementation MIInstaller
- (BOOL)performInstallationWithError:(NSError **)error
{
    ++self.stages;
    if (self.throwException)
        [NSException raise:@"fixture" format:@"interrupted install"];
    if (self.duringInstallation)
        self.duringInstallation();
    if (![self.installable performVerificationWithError:error])
        return NO;
    if (![self.installable performInstallationWithError:error])
        return NO;
    ++self.stages;
    return YES;
}
- (BOOL)_installProvisioningProfilesFromUserOptionsWithError:(NSError **)error { (void)error; return NO; }
- (BOOL)_installProvisioningProfilesAtStagingRootWithError:(NSError **)error { (void)error; return NO; }
@end

static MIExecutableBundle *bundle(Class type, NSURL *root, NSString *name, NSString *identifier)
{
    MIExecutableBundle *result = [type new];
    result.bundleURL = [root URLByAppendingPathComponent:name];
    result.identifier = identifier;
    result.infoPlistSubset = @{@"CFBundleName" : name, @"CFBundleVersion" : @"1"};
    assert([[NSFileManager defaultManager] createDirectoryAtURL:result.bundleURL withIntermediateDirectories:YES
                                                   attributes:nil error:nil]);
    uint32_t magic = 0xfeedfacf;
    assert([[NSData dataWithBytes:&magic length:sizeof(magic)] writeToURL:result.executableURL atomically:YES]);
    return result;
}


int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc > 1 && !strcmp(argv[1], "bad-abi")) {
            assert(class_addMethod(MIBundle.class, @selector(_validateExtensions:error:),
                                   (IMP)+[](id, SEL, id, id) -> uint64_t { return 0; }, "Q@:@@"));
            NSError *error = nil;
            assert(!switch_install_policy_activate(&error));
            assert(error != nil);
            return 0;
        }
        NSError *error = nil;
        assert(switch_install_policy_activate(&error));
        assert(switch_install_policy_activate(&error));
        char temporary[] = "/private/tmp/switch-install-policy.XXXXXX";
        assert(mkdtemp(temporary));
        NSURL *root = [NSURL fileURLWithPath:@(temporary) isDirectory:YES];
        switch_policy_capability_tests(root);
        switch_policy_source_tests(root);
        MIInstaller *installer = [MIInstaller new];
        installer.options = [SwitchFixtureOptions new];
        installer.stagingRoot = root;
        installer.installable = [MIInstallableBundle new];
        MIExecutableBundle *app = bundle(MIExecutableBundle.class, root, @"Example.app", @"org.example.app");
        MIAppExtensionBundle *extension = (id)bundle(MIAppExtensionBundle.class, app.bundleURL,
            @"PlugIns/Extension.appex", @"unrelated.extension.with.dots");
        MIExecutableBundle *framework = bundle(MIExecutableBundle.class, app.bundleURL,
            @"Frameworks/Example.framework", @"unrelated.framework");
        MIExtensionKitBundle *modern = (id)bundle(MIExtensionKitBundle.class, app.bundleURL,
            @"Extensions/Modern.appex", @"unrelated.modern.extension");
        installer.installable.bundle = app;
        installer.installable.children = @[ extension, framework, modern ];
        assert(![extension validateBundleMetadataWithError:nil]);
        assert([installer performInstallationWithError:&error]);
        assert(installer.stages == 2);
        assert(modern.extensionPointReady);
        assert(!installer.installable.appClip);
        for (MIExecutableBundle *item in @[ app, extension, framework ]) {
            NSDictionary *dictionary = item.codeSigningInfo.dictionary;
            assert([dictionary[@"AdHocSigned"] boolValue]);
            assert(![dictionary[@"ValidatedByProfile"] boolValue]);
            assert([dictionary[@"CodeInfoIdentifier"] isEqual:item.identifier]);
            assert(([dictionary[@"com.apple.MobileContainerManager.Entitlements"]
                isEqual:@{@"application-identifier" : item.identifier, @"keychain-access-groups" : @[item.identifier]}]));
            assert(item->_codeSignatureVerificationState == 2);
        }
        NSString *marker = [app.bundleURL.path stringByAppendingPathComponent:@".SwitchUnsigned"];
        struct stat status;
        assert(!lstat(marker.fileSystemRepresentation, &status));
        assert(S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600 && status.st_size == 0);
        assert(status.st_uid == geteuid());
        assert(![extension validateHasCorrespondingEntitlements:@{} error:nil]);
        assert(![extension validateBundleMetadataWithError:nil]);
        extension.infoPlistSubset = @{@"CFBundleName" : @"Extension"};
        assert([installer performInstallationWithError:&error]);
        extension.infoPlistSubset = @{@"CFBundleName" : @"Extension", @"CFBundleVersion" : @"1"};
        installer.options.isSystemAppInstall = YES;
        assert(![installer performInstallationWithError:&error]);
        installer.options.isSystemAppInstall = NO;
        MIInstaller *system = [MIInstaller new];
        system.options = [SwitchFixtureOptions new];
        system.options.isSystemAppInstall = YES;
        system.stagingRoot = root;
        system.installable = [MIInstallableBundle new];
        system.installable.bundle = bundle(MIExecutableBundle.class, root, @"System.app", @"com.apple.fixture");
        installer.duringInstallation = ^{
            NSError *nativeError = nil;
            assert(![system performInstallationWithError:&nativeError]);
            assert([nativeError.domain isEqualToString:@"fixture"]);
            assert(system.installable.nativeValidationCalls == 1);
            assert(system.installable.bundle.codeSigningInfo == nil);
            assert(![[NSFileManager defaultManager] fileExistsAtPath:
                [system.installable.bundle.bundleURL.path stringByAppendingPathComponent:@".SwitchUnsigned"]]);
            assert([extension validateHasNoDotInBundleIDSuffix:nil]);
            system.duringInstallation = ^{
                assert(![extension validateHasNoDotInBundleIDSuffix:nil]);
                [NSException raise:@"nested-system" format:@"interrupted native install"];
            };
            @try {
                (void)[system performInstallationWithError:nil];
                assert(false);
            } @catch (NSException *exception) {
                assert([exception.name isEqualToString:@"nested-system"]);
            }
            assert([extension validateHasNoDotInBundleIDSuffix:nil]);
        };
        assert([installer performInstallationWithError:&error]);
        installer.duringInstallation = nil;
        assert(![extension validateHasNoDotInBundleIDSuffix:nil]);
        installer.throwException = YES;
        @try {
            (void)[installer performInstallationWithError:&error];
            assert(false);
        } @catch (NSException *exception) {
            assert([exception.name isEqualToString:@"fixture"]);
        }
        assert(![extension validateHasNoDotInBundleIDSuffix:nil]);
        installer.throwException = NO;
        NSString *target = [root.path stringByAppendingPathComponent:@"target"];
        assert([@"keep" writeToFile:target atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        assert(!unlink(marker.fileSystemRepresentation));
        assert(!symlink(target.fileSystemRepresentation, marker.fileSystemRepresentation));
        assert([installer performInstallationWithError:&error]);
        assert([[NSString stringWithContentsOfFile:target encoding:NSUTF8StringEncoding error:nil] isEqual:@"keep"]);
        assert(!lstat(marker.fileSystemRepresentation, &status) && S_ISREG(status.st_mode));
        NSURL *otherRoot = [NSURL fileURLWithPath:@"/private/tmp" isDirectory:YES];
        MIExecutableBundle *outside = [MIExecutableBundle new];
        outside.bundleURL = otherRoot;
        outside.identifier = @"outside.bundle";
        installer.installable.children = @[ outside ];
        assert(![installer performInstallationWithError:&error]);
        assert(outside.codeSigningInfo == nil);
        NSURL *staging = [root URLByAppendingPathComponent:@"staging"];
        MIExecutableBundle *moved = bundle(MIExecutableBundle.class, staging, @"Moved.app", @"org.example.moved");
        installer.stagingRoot = staging;
        installer.installable.bundle = moved;
        installer.installable.children = @[];
        installer.installable.moveDestination = [root URLByAppendingPathComponent:@"containers/UUID/Moved.app"];
        assert([installer performInstallationWithError:&error]);
        NSString *movedMarker = [moved.bundleURL.path stringByAppendingPathComponent:@".SwitchUnsigned"];
        assert(!lstat(movedMarker.fileSystemRepresentation, &status));
        assert(S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600);
        MIExecutableBundle *declared = bundle(MIExecutableBundle.class, staging, @"Declared.app", @"org.example.source");
        NSData *rawSource = [NSData dataWithContentsOfURL:[root URLByAppendingPathComponent:@"UnverifiedSource"]];
        assert([rawSource writeToURL:declared.executableURL atomically:YES]);
        installer.installable.bundle = declared;
        installer.installable.moveDestination = [root URLByAppendingPathComponent:@"containers/SECOND/Declared.app"];
        assert([installer performInstallationWithError:&error]);
        NSDictionary *registration = declared.codeSigningInfo.dictionary[@"com.apple.MobileContainerManager.Entitlements"];
        assert(installer.installable.appClip);
        assert([registration[@"application-identifier"] isEqual:@"TEAM.org.example.unverified"]);
        assert([declared.codeSigningInfo.dictionary[@"TeamIdentifier"] isEqual:@"TEAM"]);
        assert([registration[@"keychain-access-groups"] isEqual:@[@"TEAM.shared"]]);
        assert(registration[@"platform-application"] == nil);
        NSData *published = [NSData dataWithContentsOfFile:[declared.executableURL.path stringByAppendingString:@".SwitchEntitlements"]];
        assert([switch_deserialize_entitlements(published) isEqual:registration]);
        assert([published isEqual:switch_serialize_entitlements(registration)]);
        assert([[NSData dataWithContentsOfURL:declared.executableURL] isEqual:rawSource]);
        assert([[NSFileManager defaultManager] removeItemAtURL:root error:nil]);
    }
    return 0;
}
