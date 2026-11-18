#import "apps/installer/native/SwitchInstallClient.h"
#import "apps/installer/native/SwitchInstallation.h"

#import <XCTest/XCTest.h>

#include <dlfcn.h>

@interface SwitchTestInstallOptions : NSObject <SwitchInstallerOptions>
@property (nonatomic, copy) NSDictionary *dictionary;
@end

@implementation SwitchTestInstallOptions

- (instancetype)initWithLegacyOptionsDictionary:(NSDictionary *)options
{
    if ((self = [super init]))
        _dictionary = [options copy];
    return self;
}

@end

@interface SwitchTestInstallIdentity : NSObject <SwitchInstallerIdentity>
@property (nonatomic, copy) NSString *identifier;
@end

@implementation SwitchTestInstallIdentity

- (instancetype)initWithBundleID:(NSString *)identifier
{
    if ((self = [super init]))
        _identifier = [identifier copy];
    return self;
}

@end

@interface SwitchTestInstallClient : NSObject <SwitchInstallerClient>
@property (nonatomic) BOOL success;
@property (nonatomic) BOOL omitCompletion;
@property (nonatomic, strong) NSError *failure;
@property (nonatomic, strong) NSURL *url;
@property (nonatomic, strong) SwitchTestInstallIdentity *identity;
@property (nonatomic) NSUInteger domain;
@property (nonatomic, strong) SwitchTestInstallOptions *options;
@property (nonatomic) BOOL returnInfo;
@end

@implementation SwitchTestInstallClient

+ (instancetype)installerWithProgressBlock:(id)progress releaseTerminationAssertionBlock:(id)releaseAssertion
{
    return [[self alloc] init];
}

- (void)installURL:(NSURL *)url
               identity:(id)identity
        targetingDomain:(NSUInteger)domain
            withOptions:(id)options
    returningResultInfo:(BOOL)returnInfo
             completion:(void (^)(BOOL, id, id, NSError *))completion
{
    self.url = url;
    self.identity = identity;
    self.domain = domain;
    self.options = options;
    self.returnInfo = returnInfo;
    if (!self.omitCompletion)
        completion(self.success, @[ @"result" ], @{@"detail" : @1}, self.failure);
}

@end

@interface SwitchInstallationTests : XCTestCase
@end

@protocol SwitchTestApplicationProxy <NSObject>
+ (instancetype)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)isInstalled;
@end

@implementation SwitchInstallationTests

- (void)testRequestUsesDefaultDomainAndExplicitBundleIdentifier
{
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    client.success = YES;
    NSURL *url = [NSURL fileURLWithPath:@"/package.ipa"];
    NSError *failure = nil;
    XCTAssertTrue(switch_install_with_client(client, url, @"com.example.fixture", [SwitchTestInstallOptions class],
                                             [SwitchTestInstallIdentity class], &failure));
    XCTAssertNil(failure);
    XCTAssertEqualObjects(client.url, url);
    XCTAssertEqualObjects(client.identity.identifier, @"com.example.fixture");
    XCTAssertEqual(client.domain, 1u);
    XCTAssertFalse(client.returnInfo);
    XCTAssertEqualObjects(client.options.dictionary, (@{
                              @"PackageType" : @"Customer",
                              @"CFBundleIdentifier" : @"com.example.fixture",
                              @"IsUserInitiated" : @YES,
                          }));
}

- (void)testDaemonErrorAndUnderlyingErrorArePreserved
{
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    NSError *underlying = [NSError errorWithDomain:NSPOSIXErrorDomain code:13 userInfo:nil];
    client.failure = [NSError errorWithDomain:@"MIInstallerErrorDomain"
                                         code:7
                                     userInfo:@{NSUnderlyingErrorKey : underlying}];
    NSError *failure = nil;
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              &failure));
    XCTAssertEqual(failure, client.failure);
    XCTAssertEqual(failure.userInfo[NSUnderlyingErrorKey], underlying);
}

- (void)testFalseResultWithoutErrorIsStillFailure
{
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    NSError *failure = nil;
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              &failure));
    XCTAssertNotNil(failure);
}

- (void)testSuccessWithErrorDoesNotReportInstalled
{
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    client.success = YES;
    client.failure = [NSError errorWithDomain:@"MIInstallerErrorDomain" code:7 userInfo:nil];
    NSError *failure = nil;
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              &failure));
    XCTAssertEqual(failure, client.failure);
}

- (void)testMissingCompletionDoesNotReportInstalled
{
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    client.omitCompletion = YES;
    NSError *failure = nil;
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              &failure));
    XCTAssertNotNil(failure);
}

- (void)testInvalidRequestAndUnavailableClient
{
    NSError *failure = nil;
    XCTAssertFalse(switch_install_with_client(nil, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              &failure));
    XCTAssertNotNil(failure);
    SwitchTestInstallClient *client = [[SwitchTestInstallClient alloc] init];
    client.success = YES;
    XCTAssertFalse(switch_install_with_client(client, [NSURL URLWithString:@"https://example.com/package.ipa"],
                                              @"com.example.fixture", [SwitchTestInstallOptions class],
                                              [SwitchTestInstallIdentity class], nil));
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"",
                                              [SwitchTestInstallOptions class], [SwitchTestInstallIdentity class],
                                              nil));
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              Nil, [SwitchTestInstallIdentity class], nil));
    XCTAssertFalse(switch_install_with_client(client, [NSURL fileURLWithPath:@"/package.ipa"], @"com.example.fixture",
                                              [SwitchTestInstallOptions class], Nil, nil));
}

- (void)testRuntimeContractAndMissingArchiveReachMobileInstallation
{
    void *library =
        dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW);
    XCTAssertNotEqual(library, nullptr);
    if (!library)
        return;
    using SwitchDomainString = NSString *(*)(NSUInteger);
    auto domainString = reinterpret_cast<SwitchDomainString>(dlsym(library, "MIStringForInstallationDomain"));
    XCTAssertNotEqual(domainString, nullptr);
    if (domainString)
        XCTAssertEqualObjects(domainString(1), @"MIInstallationDomainDefault");
    Class<SwitchInstallerClient> clientType = NSClassFromString(@"MIInstallerClient");
    Class<SwitchInstallerOptions> optionsType = NSClassFromString(@"MIInstallOptions");
    Class<SwitchInstallerIdentity> identityType = NSClassFromString(@"MIAppIdentity");
    XCTAssertTrue(
        [clientType respondsToSelector:@selector(installerWithProgressBlock:releaseTerminationAssertionBlock:)]);
    XCTAssertTrue([optionsType instancesRespondToSelector:@selector(initWithLegacyOptionsDictionary:)]);
    XCTAssertTrue([identityType instancesRespondToSelector:@selector(initWithBundleID:)]);
    if (!clientType || !optionsType || !identityType)
        return;
    XCTestExpectation *finished = [self expectationWithDescription:@"MobileInstallation reply"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        id<SwitchInstallerClient> client = [clientType installerWithProgressBlock:nil
                                                 releaseTerminationAssertionBlock:nil];
        NSError *failure = nil;
        NSURL *url = [[NSURL fileURLWithPath:NSTemporaryDirectory()]
            URLByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingPathExtension:@"ipa"]];
        BOOL success = switch_install_with_client(client, url, @"com.brunostjohn.switch.installfixture", optionsType,
                                                  identityType, &failure);
        XCTAssertFalse(success);
        XCTAssertNotNil(failure);
        XCTAssertFalse([failure.domain isEqualToString:NSOSStatusErrorDomain] && failure.code == -4);
        XCTAssertFalse([failure.domain isEqualToString:@"com.brunostjohn.switch.installer"]);
        XCTAssertNotEqualObjects(failure.localizedDescription, @"Unknown location type");
        NSLog(@"MobileInstallation missing-archive result: %@", failure);
        [finished fulfill];
    });
    [self waitForExpectations:@[ finished ] timeout:30];
}

- (void)testRuntimeInstallsKnownSimulatorIPA
{
    void *library =
        dlopen("/System/Library/PrivateFrameworks/MobileInstallation.framework/MobileInstallation", RTLD_NOW);
    XCTAssertNotEqual(library, nullptr);
    NSURL *archive = [[NSBundle bundleForClass:self.class] URLForResource:@"install_fixture" withExtension:@"ipa"];
    XCTAssertNotNil(archive);
    if (!library || !archive)
        return;
    XCTestExpectation *finished = [self expectationWithDescription:@"Known IPA installation"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        NSError *failure = nil;
        __block BOOL receivedPercent = NO;
        BOOL success = [SwitchInstallation installURL:archive
                                           identifier:@"com.brunostjohn.switch.installfixture"
                                             progress:^(NSString *status, NSNumber *percent) {
                                                 if (percent && percent.doubleValue >= 0 && percent.doubleValue <= 100)
                                                     receivedPercent = YES;
                                             }
                                                error:&failure];
        XCTAssertTrue(success, @"%@", failure);
        XCTAssertTrue(receivedPercent, @"The install service did not report a percentage");
        XCTAssertNil(failure);
        if (success) {
            dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
            Class<SwitchTestApplicationProxy> proxyType = NSClassFromString(@"LSApplicationProxy");
            XCTAssertTrue(
                [[proxyType applicationProxyForIdentifier:@"com.brunostjohn.switch.installfixture"] isInstalled]);
        }
        [finished fulfill];
    });
    [self waitForExpectations:@[ finished ] timeout:60];
}

@end
