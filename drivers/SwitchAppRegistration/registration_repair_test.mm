#import "app_registration.h"
#include <gtest/gtest.h>
#include <os/log.h>

static os_log_t logger = OS_LOG_DEFAULT;
static const int64_t kSaveTimeout = 0;
static NSMutableDictionary<NSString *, NSString *> *switch_records;
static unsigned switch_requests;
enum class SwitchSaveResult {
    saved,
    rejected,
    failed,
    silent,
    stale
};
static SwitchSaveResult switch_save_result;

BOOL switch_app_registered_at_path(NSString *path, NSString *identifier)
{
    return [switch_records[path] isEqualToString:identifier];
}

BOOL switch_register_builtin_app(NSString *path, NSUUID *uuid, id<LSOperationSaveObserver> observer, NSError **error)
{
    ++switch_requests;
    if (switch_save_result == SwitchSaveResult::rejected) {
        *error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteUnknownError userInfo:nil];
        return NO;
    }
    if (switch_save_result == SwitchSaveResult::saved) {
        NSDictionary *info =
            [NSDictionary dictionaryWithContentsOfFile:[path stringByAppendingPathComponent:@"Info.plist"]];
        switch_records[path] = info[@"CFBundleIdentifier"];
    }
    if (switch_save_result == SwitchSaveResult::failed)
        [observer operationWithUUID:uuid
             didFailToSaveWithError:[NSError errorWithDomain:NSCocoaErrorDomain
                                                        code:NSFileWriteUnknownError
                                                    userInfo:nil]];
    else if (switch_save_result != SwitchSaveResult::silent)
        [observer operationWithUUIDWasSaved:uuid];
    return YES;
}

#include "registration_repair.h"

class SwitchRegistrationRepair : public testing::Test {
protected:
    NSString *root;
    NSString *overlay;
    NSString *applications;

    void SetUp() override
    {
        root = [NSTemporaryDirectory() stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        overlay = [root stringByAppendingPathComponent:@"overlay"];
        applications = [root stringByAppendingPathComponent:@"Applications"];
        ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:overlay
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        ASSERT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:applications
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        switch_records = [NSMutableDictionary new];
        switch_requests = 0;
        switch_save_result = SwitchSaveResult::saved;
    }

    void TearDown() override
    {
        [[NSFileManager defaultManager] removeItemAtPath:root error:nil];
        switch_records = nil;
    }

    NSString *add(NSString *name, NSString *identifier)
    {
        NSString *path = [applications stringByAppendingPathComponent:name];
        EXPECT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:path
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        EXPECT_TRUE([[NSFileManager defaultManager] createDirectoryAtPath:[overlay stringByAppendingPathComponent:name]
                                              withIntermediateDirectories:YES
                                                               attributes:nil
                                                                    error:nil]);
        EXPECT_TRUE([@{@"CFBundleIdentifier" : identifier}
            writeToFile:[path stringByAppendingPathComponent:@"Info.plist"]
             atomically:YES]);
        return path;
    }
};

TEST_F(SwitchRegistrationRepair, RepairsMissingAndMismatchedRecordsOnce)
{
    NSString *first = add(@"First.app", @"com.brunostjohn.switch.first");
    NSString *second = add(@"Second.app", @"com.brunostjohn.switch.second");
    switch_records[second] = @"com.brunostjohn.switch.old";
    EXPECT_EQ(repairRegistrations(overlay, applications), 0u);
    EXPECT_EQ(switch_requests, 2u);
    EXPECT_TRUE(switch_app_registered_at_path(first, @"com.brunostjohn.switch.first"));
    EXPECT_EQ(repairRegistrations(overlay, applications), 0u);
    EXPECT_EQ(switch_requests, 2u);
}

TEST_F(SwitchRegistrationRepair, FailedAndUnacknowledgedRepairsCanBeRetried)
{
    add(@"First.app", @"com.brunostjohn.switch.first");
    for (auto result :
         {SwitchSaveResult::rejected, SwitchSaveResult::failed, SwitchSaveResult::silent, SwitchSaveResult::stale}) {
        switch_save_result = result;
        EXPECT_EQ(repairRegistrations(overlay, applications), 1u);
    }
    switch_save_result = SwitchSaveResult::saved;
    EXPECT_EQ(repairRegistrations(overlay, applications), 0u);
    EXPECT_EQ(switch_requests, 5u);
}

TEST_F(SwitchRegistrationRepair, MissingOverlayIsEmptyButUnreadableOverlayFails)
{
    EXPECT_EQ(repairRegistrations([root stringByAppendingPathComponent:@"absent"], applications), 0u);
    NSString *file = [root stringByAppendingPathComponent:@"file"];
    ASSERT_TRUE([[NSData data] writeToFile:file atomically:YES]);
    EXPECT_EQ(repairRegistrations(file, applications), 1u);
}

TEST_F(SwitchRegistrationRepair, MissingVisibleBundleDoesNotSilentlySucceed)
{
    NSString *path = add(@"First.app", @"com.brunostjohn.switch.first");
    ASSERT_TRUE([[NSFileManager defaultManager] removeItemAtPath:path error:nil]);
    EXPECT_EQ(repairRegistrations(overlay, applications), 1u);
    EXPECT_EQ(switch_requests, 0u);
}

TEST_F(SwitchRegistrationRepair, ObserverSettlesEachOperationOnce)
{
    SwitchSaveObserver *observer = [SwitchSaveObserver new];
    NSUUID *uuid = NSUUID.UUID;
    [observer expect:uuid token:@"first"];
    [observer operationWithUUIDWasSaved:uuid];
    [observer operationWithUUIDWasSaved:uuid];
    [observer cancel:uuid];
    EXPECT_EQ([observer savedBy:DISPATCH_TIME_NOW].count, 1u);
}
