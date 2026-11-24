#import "app_registration.mm"
#include <gtest/gtest.h>

// No test here registers a package; bootstrap_apps_test covers package_records.mm.
NSArray<NSDictionary *> *switch_package_install_info(NSArray<NSDictionary *> *, NSError **)
{
    return nil;
}

static NSMutableDictionary<NSString *, NSURL *> *switch_test_urls;
static NSString *switch_test_removed_identifier;
static NSURL *switch_test_removed_url;
static unsigned switch_test_operation;

@interface SwitchRegistrationProxyFixture : NSObject
@property NSURL *bundleURL;
@end

@implementation SwitchRegistrationProxyFixture

+ (id)applicationProxyForIdentifier:(NSString *)identifier
{
    SwitchRegistrationProxyFixture *proxy = [self new];
    proxy.bundleURL = switch_test_urls[identifier];
    return proxy;
}

@end

@interface SwitchRegistrationContextFixture : NSObject
@property NSNumber *targetUserID;
@end

@implementation SwitchRegistrationContextFixture
@end

@interface SwitchRegistrationWorkspaceFixture : NSObject
@end

@implementation SwitchRegistrationWorkspaceFixture

+ (id)defaultWorkspace
{
    return [self new];
}

- (BOOL)unregisterContainerizedApplicationWithBundleIdentifier:(NSString *)identifier
                                                 operationUUID:(NSUUID *)uuid
                                       unregistrationOperation:(unsigned int)operation
                                                  precondition:(id)precondition
                                                requestContext:(id)context
                                                  saveObserver:(id<LSOperationSaveObserver>)observer
                                           unregistrationError:(NSError **)error
{
    switch_test_removed_identifier = identifier;
    switch_test_operation = operation;
    return YES;
}

- (BOOL)unregisterBuiltinApplicationAtURL:(NSURL *)url
                            operationUUID:(NSUUID *)uuid
                           requestContext:(id)context
                             saveObserver:(id<LSOperationSaveObserver>)observer
                                    error:(NSError **)error
{
    switch_test_removed_url = url;
    return YES;
}

@end

class SwitchRegistrationAPI : public testing::Test {
protected:
    NSString *path;
    NSString *identifier = @"com.brunostjohn.switch.fixture";

    void SetUp() override
    {
        path = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        switch_test_urls = [NSMutableDictionary new];
        switch_test_removed_identifier = nil;
        switch_test_removed_url = nil;
        switch_test_operation = 0;
        proxyClass = (Class<SwitchLSApplicationProxy>)SwitchRegistrationProxyFixture.class;
        workspaceClass = (Class<SwitchLSApplicationWorkspace>)SwitchRegistrationWorkspaceFixture.class;
        contextClass = SwitchRegistrationContextFixture.class;
    }

    void TearDown() override
    {
        [[NSFileManager defaultManager] removeItemAtPath:path error:nil];
    }
};

TEST_F(SwitchRegistrationAPI, RemovedBundleStillHasARegistration)
{
    EXPECT_FALSE(switch_app_registered_at_path(path, identifier));
    switch_test_urls[identifier] = [NSURL fileURLWithPath:path isDirectory:YES];
    EXPECT_TRUE(switch_app_registered_at_path(path, identifier));
    EXPECT_TRUE(switch_unregister_builtin_app(path, identifier, NSUUID.UUID, nil, nil));
    EXPECT_TRUE([switch_test_removed_identifier isEqualToString:identifier]);
    EXPECT_EQ(switch_test_operation, 3u);
    EXPECT_EQ(switch_test_removed_url, nil);
}

TEST_F(SwitchRegistrationAPI, ContainerReplacementDoesNotMatchTheRemovedOverlay)
{
    switch_test_urls[identifier] =
        [NSURL fileURLWithPath:@"/private/var/containers/Bundle/Application/container/Fixture.app"];
    EXPECT_FALSE(switch_app_registered_at_path(path, identifier));
}

TEST_F(SwitchRegistrationAPI, ExistingBundleUsesTheVersionsNativeUnregistration)
{
    ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:path
                                          withIntermediateDirectories:YES
                                                           attributes:nil
                                                                error:nil]);
    EXPECT_TRUE(switch_unregister_builtin_app(path, identifier, NSUUID.UUID, nil, nil));
#if SWITCH_APPREGD_INSTALLATION_RECORDS
    EXPECT_TRUE([switch_test_removed_url.path isEqualToString:path]);
    EXPECT_EQ(switch_test_removed_identifier, nil);
#else
    EXPECT_TRUE([switch_test_removed_identifier isEqualToString:identifier]);
    EXPECT_EQ(switch_test_removed_url, nil);
#endif
}
