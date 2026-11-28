// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <os/log.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "drivers/SwitchBootstrap/migration_state.h"
#include "drivers/SwitchBootstrap/process.h"

static NSString *switch_test_base;
static NSString *switch_test_sentinel;
static NSString *switch_test_marker;
static NSMutableArray<NSString *> *switch_test_calls;
static SwitchMigrationStatus switch_test_receipt;
static NSString *switch_test_phase;
static int switch_test_recorded_error;
static unsigned switch_test_attempt;
static NSString *switch_test_failure;
static int switch_test_failure_code;
static bool switch_test_receipt_corrupt;
static bool switch_test_expire_runtime;
static int switch_test_recovery;
static int switch_test_ready;
static int switch_test_home;
static unsigned switch_test_budget;
static std::vector<unsigned> switch_test_budgets;
static int switch_test_event_error;
static std::chrono::steady_clock::time_point switch_test_deadline;
static int switch_test_notify = -1;
static unsigned switch_test_watches;
static bool switch_test_replace_on_watch;
static NSString *const switch_test_generation = @"a81182d81f619b53a798f4effe0b5a0caa3ab315df3ff916d3a5bbada6467b28";

static const char *switch_test_path(const char *path)
{
    if (!strcmp(path, "/private/var/datamigrator/migrate"))
        return switch_test_sentinel.fileSystemRepresentation;
    if (!strcmp(path, "/private/var/Switch/system-app-migration"))
        return switch_test_marker.fileSystemRepresentation;
    return path;
}

static int switch_test_kevent(int queue, const struct kevent *changes, int count, struct kevent *events, int capacity,
                              const struct timespec *timeout)
{
    assert(!timeout);
    if (!count && capacity && switch_test_event_error) {
        errno = switch_test_event_error;
        return -1;
    }
    if (switch_test_notify >= 0 && !count && capacity) {
        switch_test_deadline = std::chrono::steady_clock::now();
        assert(switch_test_receipt == SwitchMigrationWorking && [switch_test_phase isEqual:@"native-migration"]);
        assert(write(switch_test_notify, "W", 1) == 1);
    }
    const int result = kevent(queue, changes, count, events, capacity, timeout);
    if (result >= 0 && count && changes[0].filter == EVFILT_VNODE) {
        ++switch_test_watches;
        if (switch_test_replace_on_watch) {
            if (switch_test_watches >= 4) {
                assert(switch_test_watches == 4 && !unlink(switch_test_sentinel.fileSystemRepresentation));
                return result;
            }
            NSString *replacement = [switch_test_base stringByAppendingPathComponent:@"native-replacement"];
            const int file = open(replacement.fileSystemRepresentation, O_WRONLY | O_CREAT | O_TRUNC, 0600);
            assert(file >= 0 && !close(file));
            assert(!rename(replacement.fileSystemRepresentation, switch_test_sentinel.fileSystemRepresentation));
        }
    }
    return result;
}

#define open(path, ...)   open(switch_test_path(path), __VA_ARGS__)
#define lstat(path, info) lstat(switch_test_path(path), info)
#define unlink(path)      unlink(switch_test_path(path))
#define kevent(...)       switch_test_kevent(__VA_ARGS__)
#undef os_log_error
#define os_log_error(...) ((void)0)
#include "drivers/SwitchBootstrap/migration_boot.mm"
#undef open
#undef lstat
#undef unlink
#undef kevent
#undef os_log_error

static int switch_test_record(NSString *call)
{
    [switch_test_calls addObject:call];
    return [call isEqual:switch_test_failure] ? switch_test_failure_code : 0;
}

extern "C" void switch_bootstrap_set_budget(unsigned milliseconds)
{
    switch_test_budget = milliseconds;
    switch_test_budgets.push_back(milliseconds);
    switch_test_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
}

extern "C" unsigned switch_bootstrap_remaining_ms(void)
{
    const auto left = switch_test_deadline - std::chrono::steady_clock::now();
    return left <= decltype(left)::zero() ? 0 : (unsigned)std::chrono::ceil<std::chrono::milliseconds>(left).count();
}

int switch_bootstrap_read_flag(const char *name, int *flag)
{
    if (switch_test_failure && [switch_test_failure isEqual:@(name)])
        return ENOTSUP;
    if (!strcmp(name, "security.mac.switch_no_tweaks"))
        *flag = switch_test_recovery;
    else if (!strcmp(name, "security.mac.switch_bootstrap_ready"))
        *flag = switch_test_ready;
    else {
        assert(!strcmp(name, "kern.switch_migration_home_ready"));
        *flag = switch_test_home;
    }
    return 0;
}

int switch_bootstrap_readiness(int ready)
{
    const int error = switch_test_record(ready ? @"ready" : @"not-ready");
    if (!error)
        switch_test_ready = ready;
    return error;
}

int switch_migration_read(NSString *, NSString *generation, SwitchMigrationStatus *status, NSString **phase,
                          int *recordedError, unsigned *attempt)
{
    assert([generation isEqual:switch_test_generation]);
    *status = switch_test_receipt;
    const bool active = *status == SwitchMigrationWorking || *status == SwitchMigrationFailed;
    if (phase)
        *phase = active ? switch_test_phase : nil;
    if (recordedError)
        *recordedError = *status == SwitchMigrationFailed ? switch_test_recorded_error : 0;
    if (attempt)
        *attempt = active ? switch_test_attempt : 0;
    return switch_test_receipt_corrupt ? EINVAL : 0;
}

int switch_migration_write(NSString *, NSString *generation, SwitchMigrationStatus status, const char *phase, int error,
                           unsigned attempt)
{
    assert([generation isEqual:switch_test_generation]);
    assert(attempt >= 1);
    NSString *record;
    switch (status) {
    case SwitchMigrationWorking:
        assert(!error && phase);
        record = [@"working:" stringByAppendingString:@(phase)];
        break;
    case SwitchMigrationFailed:
        assert(error && phase);
        record = [@"failed:" stringByAppendingString:@(phase)];
        break;
    case SwitchMigrationComplete:
        assert(!error && !phase && attempt == 1);
        record = @"complete";
        break;
    case SwitchMigrationRepair:
        assert(!error && !phase && attempt == 1);
        record = @"repair";
        break;
    default:
        assert(false);
        return EINVAL;
    }
    const int result = switch_test_record(record);
    if (!result) {
        switch_test_receipt = status;
        switch_test_phase = phase ? @(phase) : nil;
        switch_test_recorded_error = error;
        switch_test_attempt = (status == SwitchMigrationWorking || status == SwitchMigrationFailed) ? attempt : 0;
    }
    return result;
}

int switch_bootstrap_prepare_packages(NSString *, NSString *)
{
    assert(!switch_test_recovery);
    return switch_test_record(@"packages");
}

int switch_bootstrap_prepare_repair(NSString *, NSString *)
{
    assert(switch_test_recovery);
    return switch_test_record(@"repair-root");
}

int switch_bootstrap_validate_root(NSString *, BOOL prepared)
{
    return switch_test_record(prepared ? @"validate-prepared" : @"validate");
}

int switch_bootstrap_register_apps(BOOL repair)
{
    assert(access(switch_test_sentinel.fileSystemRepresentation, F_OK) < 0 && errno == ENOENT);
    assert(switch_bootstrap_remaining_ms() > 0);
    return switch_test_record(repair ? @"repair-register" : @"register");
}

int switch_bootstrap_start_runtime(NSString *, NSMutableArray *loaded)
{
    assert(!switch_test_recovery);
    [loaded addObject:@"new-job.plist"];
    if (switch_test_expire_runtime)
        switch_bootstrap_set_budget(0);
    return switch_test_record(@"runtime");
}

void switch_bootstrap_unload_daemons(NSArray<NSString *> *loaded)
{
    if (loaded.count)
        assert(!switch_test_record(@"rollback"));
}

static void switch_test_reset(SwitchMigrationStatus receipt = SwitchMigrationMissing)
{
    switch_test_calls = [NSMutableArray new];
    switch_test_receipt = receipt;
    switch_test_phase = @"preparation";
    switch_test_recorded_error = 0;
    switch_test_attempt = (receipt == SwitchMigrationWorking || receipt == SwitchMigrationFailed) ? 1 : 0;
    switch_test_failure = nil;
    switch_test_failure_code = EIO;
    switch_test_receipt_corrupt = false;
    switch_test_expire_runtime = false;
    switch_test_recovery = 0;
    switch_test_ready = 0;
    switch_test_home = 0;
    switch_test_budgets.clear();
    unlink(switch_test_sentinel.fileSystemRepresentation);
    unlink(switch_test_marker.fileSystemRepresentation);
    unlink([switch_test_base stringByAppendingPathComponent:@"migration.native"].fileSystemRepresentation);
    NSString *request = [switch_test_base stringByAppendingPathComponent:@"migration.pending"];
    assert([@"request" writeToFile:request atomically:YES encoding:NSUTF8StringEncoding error:nil]);
}

static int switch_test_run(void)
{
    const int result = switch_migration_boot(switch_test_base, @"root", switch_test_generation);
    NSString *user = [switch_test_base stringByAppendingPathComponent:@"user-package"];
    assert([[NSString stringWithContentsOfFile:user encoding:NSUTF8StringEncoding error:nil] isEqual:@"keep"]);
    return result;
}

static void switch_test_registration_resume(void)
{
    for (auto receipt : {SwitchMigrationWorking, SwitchMigrationFailed}) {
        switch_test_reset(receipt);
        switch_test_phase = @"registration";
        switch_test_recorded_error = receipt == SwitchMigrationFailed ? ECANCELED : 0;
        switch_test_home = 1;
        assert(!switch_test_run());
        assert(([switch_test_calls isEqual:@[
            @"working:registration", @"validate-prepared", @"not-ready", @"register", @"working:runtime", @"runtime",
            @"ready", @"complete"
        ]]));
    }

    switch_test_reset(SwitchMigrationFailed);
    switch_test_phase = @"registration";
    switch_test_recorded_error = EIO;
    switch_test_home = 1;
    assert(switch_test_run() == ECANCELED);
    assert(([switch_test_calls isEqual:@[ @"not-ready" ]]));

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"registration";
    switch_test_home = 1;
    switch_test_failure = @"register";
    switch_test_failure_code = ECANCELED;
    assert(switch_test_run() == ECANCELED);
    assert(switch_test_receipt == SwitchMigrationFailed && [switch_test_phase isEqual:@"register-apps"] &&
           switch_test_recorded_error == ECANCELED && switch_test_attempt == 2);
    [switch_test_calls removeAllObjects];
    assert(switch_test_run() == ECANCELED);
    assert(([switch_test_calls isEqual:@[ @"not-ready" ]]));
}

static void switch_test_interrupted_retries(void)
{
    NSString *pending = [switch_test_base stringByAppendingPathComponent:@"migration.pending"];
    NSString *native = [switch_test_base stringByAppendingPathComponent:@"migration.native"];
    NSFileManager *files = NSFileManager.defaultManager;

    for (auto receipt : {SwitchMigrationWorking, SwitchMigrationFailed}) {
        switch_test_reset(receipt);
        switch_test_phase = @"preparation";
        switch_test_recorded_error = receipt == SwitchMigrationFailed ? ECANCELED : 0;
        assert(!switch_test_run());
        assert(([switch_test_calls isEqual:@[
            @"working:preparation", @"packages", @"not-ready", @"working:native-migration", @"working:registration",
            @"register", @"working:runtime", @"runtime", @"ready", @"complete"
        ]]));
        assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
        assert(![files fileExistsAtPath:pending]);
    }

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"preparation";
    switch_test_home = 1;
    unlink(pending.fileSystemRepresentation);
    assert(switch_test_run() == ECANCELED);
    assert(([switch_test_calls isEqual:@[ @"not-ready", @"failed:preparation" ]]));
    assert(switch_test_receipt == SwitchMigrationFailed && switch_test_recorded_error == ECANCELED &&
           switch_test_attempt == 1);
    assert([[NSString stringWithContentsOfFile:pending encoding:NSASCIIStringEncoding
                                         error:nil] isEqual:[switch_test_generation stringByAppendingString:@"\n"]]);
    switch_test_home = 0;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:pending]);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"runtime";
    switch_test_home = 1;
    assert(!switch_test_run());
    assert(([switch_test_calls
        isEqual:@[ @"working:runtime", @"validate-prepared", @"not-ready", @"runtime", @"ready", @"complete" ]]));
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"runtime";
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[
        @"working:runtime", @"validate-prepared", @"not-ready", @"register", @"runtime", @"ready", @"complete"
    ]]));

    switch_test_reset();
    switch_test_home = 1;
    unlink(pending.fileSystemRepresentation);
    assert(switch_test_run() == ECANCELED);
    assert(([switch_test_calls isEqual:@[ @"failed:unscheduled" ]]));
    assert(switch_test_attempt == 1);
    assert([[NSString stringWithContentsOfFile:pending encoding:NSASCIIStringEncoding
                                         error:nil] isEqual:[switch_test_generation stringByAppendingString:@"\n"]]);
    switch_test_home = 0;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);

    // Repeated unscheduled deferrals never delete migration.pending or permanently fail-close.
    switch_test_reset();
    switch_test_home = 1;
    for (unsigned boot = 1; boot <= switch_migration_max_attempts + 2; ++boot) {
        [switch_test_calls removeAllObjects];
        assert(switch_test_run() == ECANCELED);
        assert(switch_test_receipt == SwitchMigrationFailed && [switch_test_phase isEqual:@"unscheduled"] &&
               switch_test_recorded_error == ECANCELED && switch_test_attempt == 1);
        assert([files fileExistsAtPath:pending]);
    }
    switch_test_home = 0;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:pending]);

    // Self-heal a device stuck in legacy failed unscheduled 89 3 state.
    switch_test_reset(SwitchMigrationFailed);
    switch_test_phase = @"unscheduled";
    switch_test_recorded_error = ECANCELED;
    switch_test_attempt = switch_migration_max_attempts;
    switch_test_home = 1;
    unlink(pending.fileSystemRepresentation);
    assert(switch_test_run() == ECANCELED);
    assert(switch_test_receipt == SwitchMigrationFailed && [switch_test_phase isEqual:@"unscheduled"] &&
           switch_test_attempt == 1);
    assert([files fileExistsAtPath:pending]);
    switch_test_home = 0;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);

    // ETIMEDOUT during preparation re-arms migration.pending and retries up to switch_migration_max_attempts.
    switch_test_reset();
    switch_test_failure = @"packages";
    switch_test_failure_code = ETIMEDOUT;
    for (unsigned expected = 1; expected <= switch_migration_max_attempts; ++expected) {
        [switch_test_calls removeAllObjects];
        assert(switch_test_run() == ETIMEDOUT);
        assert(switch_test_receipt == SwitchMigrationFailed && [switch_test_phase isEqual:@"preparation"] &&
               switch_test_recorded_error == ETIMEDOUT && switch_test_attempt == expected);
        assert([files fileExistsAtPath:pending] == (expected < switch_migration_max_attempts));
    }
    [switch_test_calls removeAllObjects];
    switch_test_failure = nil;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"not-ready", @"validate", @"repair-register" ]]));
    assert(switch_test_receipt == SwitchMigrationFailed && !switch_test_ready);

    // ETIMEDOUT during preparation recovers cleanly on the next boot.
    switch_test_reset();
    switch_test_failure = @"packages";
    switch_test_failure_code = ETIMEDOUT;
    assert(switch_test_run() == ETIMEDOUT);
    assert([files fileExistsAtPath:pending] && switch_test_attempt == 1);
    switch_test_failure = nil;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:pending]);

    // ETIMEDOUT during register-apps resumes at registration instead of re-extracting packages.
    switch_test_reset();
    switch_test_failure = @"register";
    switch_test_failure_code = ETIMEDOUT;
    assert(switch_test_run() == ETIMEDOUT);
    assert(switch_test_receipt == SwitchMigrationFailed && [switch_test_phase isEqual:@"register-apps"] &&
           switch_test_recorded_error == ETIMEDOUT && switch_test_attempt == 1);
    assert([files fileExistsAtPath:pending]);
    switch_test_failure = nil;
    switch_test_home = 1;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[
        @"working:registration", @"validate-prepared", @"not-ready", @"register", @"working:runtime", @"runtime",
        @"ready", @"complete"
    ]]));
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:pending]);

    // ETIMEDOUT during runtime re-arms migration.pending and resumes at runtime.
    switch_test_reset();
    switch_test_expire_runtime = true;
    assert(switch_test_run() == ETIMEDOUT);
    assert([files fileExistsAtPath:pending]);
    switch_test_expire_runtime = false;
    switch_test_home = 1;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(([switch_test_calls
        isEqual:@[ @"working:runtime", @"validate-prepared", @"not-ready", @"runtime", @"ready", @"complete" ]]));
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:pending]);

    for (NSString *phase in @[ @"preparation", @"native-migration", @"registration", @"runtime" ]) {
        switch_test_reset(SwitchMigrationWorking);
        switch_test_phase = phase;
        switch_test_attempt = switch_migration_max_attempts;
        assert([[NSData data] writeToFile:native atomically:YES]);
        assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
        assert(!switch_test_run());
        assert(([switch_test_calls
            isEqual:@[ @"not-ready", [@"failed:" stringByAppendingString:phase], @"validate", @"repair-register" ]]));
        assert(switch_test_receipt == SwitchMigrationFailed && switch_test_attempt == switch_migration_max_attempts);
        assert(![files fileExistsAtPath:pending] && ![files fileExistsAtPath:native]);
        assert([files fileExistsAtPath:switch_test_marker]);
    }

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"preparation";
    switch_test_recovery = 1;
    assert([[NSData data] writeToFile:native atomically:YES]);
    assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"repair-root", @"repair-register" ]]));
    assert(switch_test_receipt == SwitchMigrationWorking && switch_test_attempt == 1);
    assert([files fileExistsAtPath:pending] && [files fileExistsAtPath:native] &&
           [files fileExistsAtPath:switch_test_marker]);
}

static void switch_test_native_rearm(void)
{
    NSString *native = [switch_test_base stringByAppendingPathComponent:@"migration.native"];
    NSFileManager *files = NSFileManager.defaultManager;

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"native-migration";
    assert([[NSData data] writeToFile:native atomically:YES]);
    assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert(![files fileExistsAtPath:native]);
    assert(![files fileExistsAtPath:switch_test_marker]);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"preparation";
    assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
    assert(!switch_test_run());
    assert(switch_test_receipt == SwitchMigrationComplete);
    assert([files fileExistsAtPath:switch_test_marker]);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"preparation";
    switch_test_home = 1;
    assert([[NSData data] writeToFile:native atomically:YES]);
    assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
    assert(switch_test_run() == ECANCELED);
    assert(switch_test_attempt == 1);
    assert(![files fileExistsAtPath:native] && ![files fileExistsAtPath:switch_test_marker]);
}

static void switch_test_transactions(void)
{
    switch_test_reset();
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[
        @"working:preparation", @"packages", @"not-ready", @"working:native-migration", @"working:registration",
        @"register", @"working:runtime", @"runtime", @"ready", @"complete"
    ]]));
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready && switch_test_budget == 450000);
    assert(![NSFileManager.defaultManager
        fileExistsAtPath:[switch_test_base stringByAppendingPathComponent:@"migration.pending"]]);

    for (NSString *failure in @[ @"packages", @"register", @"runtime", @"ready", @"complete" ]) {
        switch_test_reset();
        switch_test_failure = failure;
        assert(switch_test_run() == EIO);
        assert(switch_test_receipt == SwitchMigrationFailed && !switch_test_ready);
        assert([switch_test_calls containsObject:@"repair-register"] == [failure isEqual:@"packages"]);
        if ([switch_test_calls containsObject:@"runtime"])
            assert([switch_test_calls containsObject:@"rollback"]);
        [switch_test_calls removeAllObjects];
        switch_test_failure = nil;
        assert(!switch_test_run());
        assert(![switch_test_calls containsObject:@"packages"] && ![switch_test_calls containsObject:@"runtime"]);
        assert([switch_test_calls containsObject:@"repair-register"]);
        assert(switch_test_receipt == SwitchMigrationFailed && !switch_test_ready);
    }

    switch_test_reset(SwitchMigrationWorking);
    switch_test_attempt = switch_migration_max_attempts;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"not-ready", @"failed:preparation", @"validate", @"repair-register" ]]));
    assert(switch_test_receipt == SwitchMigrationFailed);
    switch_test_reset(SwitchMigrationWorking);
    switch_test_attempt = switch_migration_max_attempts;
    switch_test_failure = @"failed:preparation";
    assert(switch_test_run() == EIO && switch_test_receipt == SwitchMigrationWorking);
    assert([NSFileManager.defaultManager
        fileExistsAtPath:[switch_test_base stringByAppendingPathComponent:@"migration.pending"]]);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"native-migration";
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[
        @"working:native-migration", @"validate-prepared", @"not-ready", @"working:registration", @"register",
        @"working:runtime", @"runtime", @"ready", @"complete"
    ]]));

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"native-migration";
    switch_test_failure = @"validate-prepared";
    assert(switch_test_run() == EIO);
    assert(([switch_test_calls
        isEqual:@[ @"working:native-migration", @"validate-prepared", @"not-ready", @"failed:validation" ]]));

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"native-migration";
    switch_test_recovery = 1;
    assert(!switch_test_run() && switch_test_receipt == SwitchMigrationWorking);
    assert(([switch_test_calls isEqual:@[ @"not-ready", @"validate-prepared", @"repair-register" ]]));
    switch_test_recovery = 0;
    switch_test_home = 1;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run() && switch_test_receipt == SwitchMigrationComplete);
    assert([switch_test_calls containsObject:@"register"] && ![switch_test_calls containsObject:@"packages"]);

    switch_test_reset(SwitchMigrationWorking);
    switch_test_phase = @"runtime";
    switch_test_attempt = switch_migration_max_attempts;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"not-ready", @"failed:runtime", @"validate", @"repair-register" ]]));

    switch_test_reset(SwitchMigrationComplete);
    switch_test_home = 1;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"validate", @"not-ready", @"runtime", @"ready" ]]));
    assert(switch_test_budget == 450000);
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"validate" ]]));

    switch_test_reset(SwitchMigrationComplete);
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"validate", @"not-ready", @"register", @"runtime", @"ready" ]]));

    switch_test_reset(SwitchMigrationComplete);
    switch_test_recovery = 1;
    switch_test_home = 1;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"validate", @"not-ready", @"repair-register", @"repair" ]]));
    assert(switch_test_receipt == SwitchMigrationRepair && !switch_test_ready);
    switch_test_recovery = 0;
    [switch_test_calls removeAllObjects];
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"validate", @"not-ready", @"runtime", @"ready", @"complete" ]]));

    switch_test_reset();
    switch_test_recovery = 1;
    assert(!switch_test_run());
    assert(([switch_test_calls isEqual:@[ @"repair-root", @"repair-register" ]]));
    assert(switch_test_receipt == SwitchMigrationMissing && !switch_test_ready);
    assert([NSFileManager.defaultManager
        fileExistsAtPath:[switch_test_base stringByAppendingPathComponent:@"migration.pending"]]);

    switch_test_reset();
    switch_test_home = 1;
    assert(switch_test_run() == ECANCELED);
    assert(([switch_test_calls isEqual:@[ @"failed:unscheduled" ]]));
    assert(switch_test_receipt == SwitchMigrationFailed);

    switch_test_reset();
    switch_test_receipt_corrupt = true;
    assert(switch_test_run() == EINVAL);
    assert(([switch_test_calls isEqual:@[ @"not-ready" ]]));
    switch_test_reset();
    switch_test_failure = @"kern.switch_migration_home_ready";
    assert(switch_test_run() == ENOTSUP);
    assert(([switch_test_calls isEqual:@[ @"not-ready" ]]));

    switch_test_reset();
    switch_test_expire_runtime = true;
    assert(switch_test_run() == ETIMEDOUT && !switch_test_ready);
    assert([switch_test_calls containsObject:@"rollback"] && switch_test_receipt == SwitchMigrationFailed);
    assert(switch_test_budget == 15000);
    assert([switch_test_calls indexOfObject:@"failed:runtime"] < [switch_test_calls indexOfObject:@"rollback"]);
}

static bool switch_test_service_reply(unsigned holdSeconds)
{
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets))
        return false;
    const pid_t service = fork();
    char byte;
    if (!service) {
        close(sockets[0]);
        ssize_t n;
        while ((n = read(sockets[1], &byte, 1)) == 1) {
            if (byte != 'Q' || write(sockets[1], "R", 1) != 1)
                _exit(1);
        }
        _exit(n ? 1 : 0);
    }
    close(sockets[1]);
    bool replied = service > 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(holdSeconds);
    while (replied) {
        replied = write(sockets[0], "Q", 1) == 1 && read(sockets[0], &byte, 1) == 1 && byte == 'R';
        const auto now = std::chrono::steady_clock::now();
        if (!replied || now >= end)
            break;
        std::this_thread::sleep_until(std::min(end, now + std::chrono::seconds(60)));
    }
    close(sockets[0]);
    int status;
    return service > 0 && waitpid(service, &status, 0) == service && WIFEXITED(status) && !WEXITSTATUS(status) &&
           replied;
}

static void switch_test_wait(bool replace, unsigned holdSeconds = 0)
{
    switch_test_reset();
    const char *path = switch_test_sentinel.fileSystemRepresentation;
    NSString *replacement = [switch_test_base stringByAppendingPathComponent:@"replacement"];
    const char *other = replacement.fileSystemRepresentation;
    assert([[NSData data] writeToFile:switch_test_sentinel atomically:YES]);
    assert([[NSData data] writeToFile:switch_test_marker atomically:YES]);
    if (replace)
        assert([[NSData data] writeToFile:replacement atomically:YES]);
    int notify[2];
    assert(!pipe(notify));
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(notify[1]);
        char event;
        if (read(notify[0], &event, 1) != 1)
            _exit(1);
        if (!switch_test_service_reply(holdSeconds))
            _exit(4);
        if (replace) {
            if (rename(other, path) || read(notify[0], &event, 1) != 1)
                _exit(2);
        }
        _exit(unlink(path) ? 3 : 0);
    }
    close(notify[0]);
    switch_test_notify = notify[1];
    switch_test_watches = 0;
    assert(!switch_test_run());
    close(notify[1]);
    switch_test_notify = -1;
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(switch_test_watches == (replace ? 2 : 1));
    assert((switch_test_budgets == std::vector<unsigned>{450'000, 450'000}));
    assert(switch_test_receipt == SwitchMigrationComplete && switch_test_ready);
    assert([NSFileManager.defaultManager fileExistsAtPath:switch_test_marker]);
    assert(![NSFileManager.defaultManager
        fileExistsAtPath:[switch_test_base stringByAppendingPathComponent:@"migration.native"]]);
    assert(([switch_test_calls isEqual:@[
        @"working:preparation", @"packages", @"not-ready", @"working:native-migration", @"working:registration",
        @"register", @"working:runtime", @"runtime", @"ready", @"complete"
    ]]));
}

static void switch_test_native_wait(void)
{
    assert(!switch_wait_for_native_migration(switch_test_sentinel.fileSystemRepresentation));
    switch_test_wait(false);
    switch_test_wait(true);
    assert([[NSData data] writeToFile:switch_test_sentinel atomically:YES]);
    switch_test_event_error = EIO;
    switch_test_watches = 0;
    assert(switch_wait_for_native_migration(switch_test_sentinel.fileSystemRepresentation) == EIO);
    switch_test_event_error = 0;
    assert(switch_test_watches == 1);
    assert(!unlink(switch_test_sentinel.fileSystemRepresentation));
    assert([[NSData data] writeToFile:switch_test_sentinel atomically:YES]);
    switch_bootstrap_set_budget(0);
    switch_test_watches = 0;
    switch_test_replace_on_watch = true;
    assert(!switch_wait_for_native_migration(switch_test_sentinel.fileSystemRepresentation));
    switch_test_replace_on_watch = false;
    assert(switch_test_watches == 4);
    assert(!symlink("missing", switch_test_sentinel.fileSystemRepresentation));
    assert(switch_wait_for_native_migration(switch_test_sentinel.fileSystemRepresentation) == ELOOP);
    assert(!unlink(switch_test_sentinel.fileSystemRepresentation));
    assert(!mkdir(switch_test_sentinel.fileSystemRepresentation, 0700));
    assert(switch_wait_for_native_migration(switch_test_sentinel.fileSystemRepresentation) == EINVAL);
    assert(!rmdir(switch_test_sentinel.fileSystemRepresentation));
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        char temporary[] = "/private/tmp/SwitchMigrationBoot.XXXXXX";
        assert(mkdtemp(temporary));
        switch_test_base = @(temporary);
        switch_test_sentinel = [switch_test_base stringByAppendingPathComponent:@"migrate"];
        switch_test_marker = [switch_test_base stringByAppendingPathComponent:@"system-app-migration"];
        NSString *user = [switch_test_base stringByAppendingPathComponent:@"user-package"];
        assert([@"keep" writeToFile:user atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        switch_test_transactions();
        switch_test_registration_resume();
        switch_test_interrupted_retries();
        switch_test_native_rearm();
        switch_test_native_wait();
        if (argc == 2 && !strcmp(argv[1], "--long-native-migration")) {
            const auto start = std::chrono::steady_clock::now();
            fprintf(stderr, "Beginning fresh transaction with 690-second native migration\n");
            switch_test_wait(false, 690);
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start);
            assert(elapsed.count() >= 690);
            fprintf(stderr, "Fresh transaction complete after %lld seconds\n", (long long)elapsed.count());
        } else {
            assert(argc == 1);
        }
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_base error:nil]);
    }
}
