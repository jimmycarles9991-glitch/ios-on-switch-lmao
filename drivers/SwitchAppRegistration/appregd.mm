// SPDX-License-Identifier: BSD-2-Clause-Patent
// switch-appregd: registers with LaunchServices the overlay apps a deploy added or changed, and unregisters the ones it
// removed, as installd does for a built-in app (app_registration.mm), so a deploy that changes an app needs no data
// migration. The deploy lists them in kJournalPath, each entry with a token of its own (tools/deploy/app_journal.py).
//
// The deploy writes Data with owners off, so this process, which runs as mobile, cannot rewrite the journal. It keeps
// the tokens of the entries it applied in kAppliedPath, in mobile's Library, and the next deploy drops those entries.
// A token goes there only once lsd reports the operation saved (installd likewise keeps an operation in its journal
// until then): an entry whose save is not reported is applied again at the next boot.
//
// iOS 27's launchd starts it at boot from com.brunostjohn.switch.appregd.plist in the Supplemental cryptex's
// LaunchDaemons (overlay/manifest.json). iOS 17's launchd starts nothing of ours but the BlueTool stand-in, which starts
// this when launchd starts it (../SwitchBluetooth/BlueTool/BlueTool.c).
#import <Foundation/Foundation.h>

#include <os/log.h>
#include <stdlib.h>

#import "app_registration.h"

static NSString *const kJournalPath = @"/private/var/Switch/app-registration.plist";
static NSString *const kAppliedPath =
    @"/private/var/mobile/Library/com.brunostjohn.switch.app-registration.applied.plist";
static NSString *const kRegister = @"Register";
static NSString *const kUnregister = @"Unregister";
static NSString *const kIdentifier = @"BundleIdentifier";
static NSString *const kToken = @"Token";
static NSString *const kPath = @"Path";
static const int64_t kSaveTimeout = 60 * NSEC_PER_SEC;

static os_log_t logger;

@interface SwitchSaveObserver : NSObject <LSOperationSaveObserver>
- (void)expect:(NSUUID *)uuid token:(NSString *)token;
- (void)cancel:(NSUUID *)uuid;
- (NSSet<NSString *> *)savedBy:(dispatch_time_t)deadline;
@end

@implementation SwitchSaveObserver {
    NSMutableDictionary<NSUUID *, NSString *> *_pending;
    NSMutableSet<NSString *> *_saved;
    dispatch_group_t _group;
}

- (instancetype)init
{
    if ((self = [super init])) {
        _pending = [NSMutableDictionary new];
        _saved = [NSMutableSet new];
        _group = dispatch_group_create();
    }
    return self;
}

- (void)expect:(NSUUID *)uuid token:(NSString *)token
{
    @synchronized(self) {
        _pending[uuid] = token;
    }
    dispatch_group_enter(_group);
}

- (void)settle:(NSUUID *)uuid saved:(BOOL)saved
{
    NSString *token;
    @synchronized(self) {
        token = _pending[uuid];
        [_pending removeObjectForKey:uuid];
        if (token && saved)
            [_saved addObject:token];
    }
    if (token)
        dispatch_group_leave(_group);
}

- (void)cancel:(NSUUID *)uuid
{
    [self settle:uuid saved:NO];
}

- (void)operationWithUUIDWasSaved:(NSUUID *)uuid
{
    [self settle:uuid saved:YES];
}

- (void)operationWithUUID:(NSUUID *)uuid didFailToSaveWithError:(NSError *)error
{
    os_log_error(logger, "SwitchAppRegistration: lsd did not save operation %{public}@: %{public}@", uuid, error);
    [self settle:uuid saved:NO];
}

- (NSSet<NSString *> *)savedBy:(dispatch_time_t)deadline
{
    dispatch_group_wait(_group, deadline);
    @synchronized(self) {
        for (NSUUID *uuid in _pending)
            os_log_error(logger, "SwitchAppRegistration: lsd reported no save of operation %{public}@ in time", uuid);
        return [_saved copy];
    }
}

@end

// The stage's well-formed entries, by path; the deploy writes paths of /Applications/<name>.app only.
static NSArray<NSDictionary *> *entries(NSDictionary *journal, NSString *stage)
{
    NSDictionary *byPath = journal[stage];
    if (![byPath isKindOfClass:NSDictionary.class])
        return @[];
    NSMutableArray *found = [NSMutableArray new];
    for (NSString *path in [byPath.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
        NSDictionary *entry = byPath[path];
        const BOOL app = path.pathComponents.count == 3 && [path hasPrefix:@"/Applications/"] &&
                         [path.pathExtension isEqualToString:@"app"];
        if (!app || ![entry isKindOfClass:NSDictionary.class] || ![entry[kToken] isKindOfClass:NSString.class] ||
            ![entry[kIdentifier] isKindOfClass:NSString.class]) {
            os_log_error(logger, "SwitchAppRegistration: %{public}@ %{public}@: not an app entry", stage, path);
            continue;
        }
        [found addObject:@{kPath : path, kIdentifier : entry[kIdentifier], kToken : entry[kToken]}];
    }
    return found;
}

static BOOL apply(NSString *stage, NSDictionary *entry, NSUUID *uuid, SwitchSaveObserver *observer, NSError **error)
{
    if ([stage isEqualToString:kRegister])
        return switch_register_builtin_app(entry[kPath], uuid, observer, error);
    return switch_unregister_builtin_app(entry[kPath], entry[kIdentifier], uuid, observer, error);
}

static int applyJournal(void)
{
    NSDictionary *journal = [NSDictionary dictionaryWithContentsOfFile:kJournalPath];
    if (!journal)
        return EXIT_SUCCESS;
    NSError *error = nil;
    if (!switch_app_registration_load(&error)) {
        os_log_error(logger, "SwitchAppRegistration: %{public}@", error);
        return EXIT_FAILURE;
    }
    NSArray *listed = [NSArray arrayWithContentsOfFile:kAppliedPath];
    NSSet *applied = [NSSet setWithArray:listed ?: @[]];
    NSMutableSet<NSString *> *done = [NSMutableSet new];
    SwitchSaveObserver *observer = [SwitchSaveObserver new];
    unsigned failed = 0;
    // Unregistrations first: an app that changed its identifier in place is unregistered by its old one.
    for (NSString *stage in @[ kUnregister, kRegister ]) {
        for (NSDictionary *entry in entries(journal, stage)) {
            NSString *token = entry[kToken];
            if ([applied containsObject:token] ||
                ([stage isEqualToString:kUnregister] && !switch_app_installed(entry[kIdentifier]))) {
                [done addObject:token];
                continue;
            }
            NSUUID *uuid = [NSUUID UUID];
            [observer expect:uuid token:token];
            error = nil;
            if (apply(stage, entry, uuid, observer, &error)) {
                os_log(logger, "SwitchAppRegistration: %{public}@ %{public}@ (%{public}@)", stage, entry[kIdentifier],
                       entry[kPath]);
                continue;
            }
            [observer cancel:uuid];
            ++failed;
            os_log_error(logger, "SwitchAppRegistration: %{public}@ %{public}@ (%{public}@) failed: %{public}@", stage,
                         entry[kIdentifier], entry[kPath], error);
        }
    }
    [done unionSet:[observer savedBy:dispatch_time(DISPATCH_TIME_NOW, kSaveTimeout)]];
    if (![done isEqualToSet:applied]) {
        NSData *data = [NSPropertyListSerialization
            dataWithPropertyList:[done.allObjects sortedArrayUsingSelector:@selector(compare:)]
                          format:NSPropertyListXMLFormat_v1_0
                         options:0
                           error:&error];
        if (!data || ![data writeToFile:kAppliedPath
                                options:NSDataWritingAtomic | NSDataWritingFileProtectionNone
                                  error:&error]) {
            os_log_error(logger, "SwitchAppRegistration: %{public}@ not written: %{public}@", kAppliedPath, error);
            return EXIT_FAILURE;
        }
    }
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

int main(void)
{
    logger = os_log_create("com.brunostjohn.switch.appregd", "registration");
    // LaunchServices reports saves on queues of its own; the main queue stays free for any it targets there.
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        int status;
        @autoreleasepool {
            status = applyJournal();
        }
        exit(status);
    });
    dispatch_main();
}
