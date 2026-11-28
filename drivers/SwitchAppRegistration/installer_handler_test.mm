// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#include <gtest/gtest.h>

static Class switch_test_handler_class(NSString *name);

#define NSClassFromString switch_test_handler_class
#import "installer_handler.mm"
#undef NSClassFromString

static BOOL switch_test_registered;
static NSString *switch_test_bound_id;
static NSURL *switch_test_bound_url;
static NSString *switch_test_record_id;
static unsigned switch_test_set_calls;
static BOOL switch_test_set_succeeds;

BOOL switch_app_registered_at_path(NSString *, NSString *)
{
    return switch_test_registered;
}

@interface SwitchTestBoundRecord : NSObject
@property NSString *bundleIdentifier;
@property NSURL *URL;
@end
@implementation SwitchTestBoundRecord
@end

@interface SwitchTestClaimBinding : NSObject
@property SwitchTestBoundRecord *bundleRecord;
- (instancetype)initWithTypeIdentifier:(NSString *)identifier error:(NSError **)error;
@end
@implementation SwitchTestClaimBinding
- (instancetype)initWithTypeIdentifier:(NSString *)identifier error:(NSError **)error
{
    (void)error;
    if (![identifier isEqualToString:@"com.apple.itunes.ipa"] || !switch_test_bound_id)
        return nil;
    if ((self = [super init])) {
        _bundleRecord = [SwitchTestBoundRecord new];
        _bundleRecord.bundleIdentifier = switch_test_bound_id;
        _bundleRecord.URL = switch_test_bound_url;
    }
    return self;
}
@end

@interface SwitchTestApplicationRecord : NSObject
@property NSString *bundleIdentifier;
@property NSURL *URL;
- (instancetype)initWithURL:(NSURL *)url allowPlaceholder:(BOOL)placeholder error:(NSError **)error;
- (struct SwitchLSVersionNumber)_bundleVersion;
@end
@implementation SwitchTestApplicationRecord
- (instancetype)initWithURL:(NSURL *)url allowPlaceholder:(BOOL)placeholder error:(NSError **)error
{
    (void)placeholder;
    (void)error;
    if ((self = [super init])) {
        _bundleIdentifier = switch_test_record_id;
        _URL = url;
    }
    return self;
}
- (struct SwitchLSVersionNumber)_bundleVersion
{
    return {};
}
@end

@interface SwitchTestTypeRecord : NSObject
+ (instancetype)typeRecordWithIdentifier:(NSString *)identifier;
@end
@implementation SwitchTestTypeRecord
+ (instancetype)typeRecordWithIdentifier:(NSString *)identifier
{
    return [identifier isEqualToString:@"com.apple.itunes.ipa"] ? [self new] : nil;
}
@end

@interface SwitchTestWorkspace : NSObject
+ (instancetype)defaultWorkspace;
- (void)setDefaultHandlerForTypeRecord:(id)type
                   toApplicationRecord:(id)application
                     completionHandler:(void (^)(BOOL, NSError *))completion;
@end
@implementation SwitchTestWorkspace
+ (instancetype)defaultWorkspace
{
    return [self new];
}
- (void)setDefaultHandlerForTypeRecord:(id)type
                   toApplicationRecord:(id)application
                     completionHandler:(void (^)(BOOL, NSError *))completion
{
    (void)type;
    (void)application;
    ++switch_test_set_calls;
    completion(switch_test_set_succeeds, nil);
}
@end

@interface SwitchTestModifyService : NSObject
+ (id)XPCProxyWithErrorHandler:(void (^)(NSError *))handler;
- (void)setHandler:(NSString *)identifier
              version:(struct SwitchLSVersionNumber)version
                roles:(uint32_t)roles
       forContentType:(NSString *)type
    completionHandler:(void (^)(BOOL, NSError *))completion;
@end
@implementation SwitchTestModifyService
+ (id)XPCProxyWithErrorHandler:(void (^)(NSError *))handler
{
    (void)handler;
    return [self new];
}
- (void)setHandler:(NSString *)identifier
              version:(struct SwitchLSVersionNumber)version
                roles:(uint32_t)roles
       forContentType:(NSString *)type
    completionHandler:(void (^)(BOOL, NSError *))completion
{
    (void)identifier;
    (void)version;
    (void)roles;
    (void)type;
    ++switch_test_set_calls;
    completion(switch_test_set_succeeds, nil);
}
@end

static Class switch_test_handler_class(NSString *name)
{
    return (@{
        @"LSClaimBinding" : SwitchTestClaimBinding.class,
        @"LSApplicationRecord" : SwitchTestApplicationRecord.class,
        @"LSApplicationWorkspace" : SwitchTestWorkspace.class,
        @"UTTypeRecord" : SwitchTestTypeRecord.class,
        @"_LSDModifyService" : SwitchTestModifyService.class,
    })[name];
}

class SwitchInstallerHandlerTest : public testing::Test {
protected:
    NSString *bundlePath;
    NSString *identifier = @"com.brunostjohn.switch.installer";

    void SetUp() override
    {
        bundlePath = [NSTemporaryDirectory()
            stringByAppendingPathComponent:[NSUUID.UUID.UUIDString stringByAppendingString:@".app"]];
        ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:bundlePath
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        switch_test_registered = YES;
        switch_test_bound_id = nil;
        switch_test_bound_url = nil;
        switch_test_record_id = identifier;
        switch_test_set_calls = 0;
        switch_test_set_succeeds = YES;
    }

    void TearDown() override
    {
        [[NSFileManager defaultManager] removeItemAtPath:bundlePath error:nil];
    }
};

TEST_F(SwitchInstallerHandlerTest, SkipsWriteWhenCurrentHandlerAlreadyMatches)
{
    switch_test_bound_id = identifier;
    switch_test_bound_url = [NSURL fileURLWithPath:bundlePath isDirectory:YES];
    NSError *error = nil;
    EXPECT_TRUE(switch_register_installer_handler_at(bundlePath, identifier, &error));
    EXPECT_EQ(error, nil);
    EXPECT_EQ(switch_test_set_calls, 0u);
}

TEST_F(SwitchInstallerHandlerTest, WritesWhenUnboundOrPointingElsewhere)
{
    NSError *error = nil;
    EXPECT_TRUE(switch_register_installer_handler_at(bundlePath, identifier, &error));
    EXPECT_EQ(switch_test_set_calls, 1u);

    switch_test_bound_id = @"com.example.other";
    switch_test_bound_url = [NSURL fileURLWithPath:bundlePath isDirectory:YES];
    EXPECT_TRUE(switch_register_installer_handler_at(bundlePath, identifier, &error));
    EXPECT_EQ(switch_test_set_calls, 2u);

    switch_test_bound_id = identifier;
    switch_test_bound_url = [NSURL fileURLWithPath:@"/Applications/Other.app" isDirectory:YES];
    EXPECT_TRUE(switch_register_installer_handler_at(bundlePath, identifier, &error));
    EXPECT_EQ(switch_test_set_calls, 3u);
}
