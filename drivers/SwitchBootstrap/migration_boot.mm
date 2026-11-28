// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "migration_boot.h"
#import "migration_state.h"
#import "daemon_jobs.h"
#import "state.h"
#include "process.h"

#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <stdio.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr unsigned switch_migration_command_budget_ms = 450000;
static constexpr unsigned switch_migration_max_attempts = 3;
static const char switch_migration_native_sentinel[] = "/private/var/datamigrator/migrate";
static const char switch_migration_native_marker[] = "/private/var/Switch/system-app-migration";

static int switch_wait_for_native_migration(const char *path)
{
    for (;;) {
        const int sentinel = open(path, O_EVTONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (sentinel < 0)
            return errno == ENOENT ? 0 : errno;
        const int queue = kqueue();
        struct stat watched, current;
        struct kevent event;
        EV_SET(&event, sentinel, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_DELETE | NOTE_RENAME | NOTE_REVOKE, 0, NULL);
        int error = 0;
        if (queue < 0 || fstat(sentinel, &watched) || kevent(queue, &event, 1, NULL, 0, NULL) < 0) {
            error = errno;
        } else if (!S_ISREG(watched.st_mode)) {
            error = EINVAL;
        } else if (lstat(path, &current)) {
            error = errno == ENOENT ? 0 : errno;
        } else if (watched.st_ino == current.st_ino && watched.st_dev == current.st_dev) {
            const int count = kevent(queue, NULL, 0, &event, 1, NULL);
            if (count < 0 && errno != EINTR)
                error = errno;
            else if (count > 0 && (event.flags & EV_ERROR))
                error = (int)event.data;
        }
        if (queue >= 0)
            close(queue);
        close(sentinel);
        if (error)
            return error;
    }
}

static BOOL switch_migration_file_exists(const char *path)
{
    struct stat info;
    return !lstat(path, &info) && S_ISREG(info.st_mode) && info.st_nlink == 1;
}

static void switch_migration_consume(NSString *base)
{
    if (unlink([base stringByAppendingPathComponent:@"migration.pending"].fileSystemRepresentation) && errno != ENOENT)
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not clear migration request: %d", errno);
}

static int switch_migration_request(NSString *base, NSString *generation)
{
    NSString *target = [base stringByAppendingPathComponent:@"migration.pending"];
    NSString *staging = [base stringByAppendingPathComponent:@"migration.pending.partial"];
    NSData *record = [[generation stringByAppendingString:@"\n"] dataUsingEncoding:NSASCIIStringEncoding];
    if (!record || record.length != 65)
        return EINVAL;
    if (unlink(staging.fileSystemRepresentation) && errno != ENOENT)
        return errno;
    const int fd = open(staging.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0)
        return errno;
    size_t offset = 0;
    int error = 0;
    while (!error && offset < record.length) {
        const ssize_t count = write(fd, (const char *)record.bytes + offset, record.length - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (!count || errno != EINTR)
            error = count ? errno : EIO;
    }
    if (!error && fchmod(fd, 0644))
        error = errno;
    if (!error && fsync(fd))
        error = errno;
    if (close(fd) && !error)
        error = errno;
    if (!error && rename(staging.fileSystemRepresentation, target.fileSystemRepresentation))
        error = errno;
    if (error) {
        unlink(staging.fileSystemRepresentation);
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not save migration request: %d", error);
        return error;
    }
    const int directory = open(base.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory >= 0) {
        if (fsync(directory))
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not sync migration request directory: %d", errno);
        close(directory);
    }
    return 0;
}

static NSString *switch_migration_native_path(NSString *base)
{
    return [base stringByAppendingPathComponent:@"migration.native"];
}

static void switch_migration_mark_native(NSString *base)
{
    if (!switch_migration_file_exists(switch_migration_native_sentinel))
        return;
    NSString *path = switch_migration_native_path(base);
    const int fd = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not record native migration state: %d", errno);
        return;
    }
    if (fsync(fd))
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not sync native migration state: %d", errno);
    close(fd);
}

static void switch_migration_clear_native(NSString *base)
{
    if (unlink(switch_migration_native_path(base).fileSystemRepresentation) && errno != ENOENT)
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not clear native migration state: %d", errno);
}

static void switch_migration_rearm_native_if_interrupted(NSString *base)
{
    if (!switch_migration_file_exists(switch_migration_native_path(base).fileSystemRepresentation) ||
        switch_migration_file_exists(switch_migration_native_sentinel))
        return;
    // The kernel deleted an unfinished datamigrator sentinel before launchd; drop its one-try guard once to rearm it.
    switch_migration_clear_native(base);
    if (!switch_migration_file_exists(switch_migration_native_marker))
        return;
    if (unlink(switch_migration_native_marker) && errno != ENOENT)
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not rearm native migration: %d", errno);
}

static BOOL switch_migration_retryable_phase(NSString *phase)
{
    return [phase isEqualToString:@"unscheduled"] || [phase isEqualToString:@"preparation"] ||
           [phase isEqualToString:@"native-migration"] || [phase isEqualToString:@"registration"] ||
           [phase isEqualToString:@"runtime"];
}

static BOOL switch_migration_timeout_retryable_phase(NSString *phase)
{
    return switch_migration_retryable_phase(phase) || [phase isEqualToString:@"register-apps"];
}

static BOOL switch_migration_needs_preparation(NSString *phase)
{
    return [phase isEqualToString:@"unscheduled"] || [phase isEqualToString:@"preparation"];
}

static int switch_migration_wait(void)
{
    const int error = switch_wait_for_native_migration(switch_migration_native_sentinel);
    if (!error)
        switch_bootstrap_set_budget(switch_migration_command_budget_ms);
    return error;
}

static int switch_migration_register_repair(NSString *root)
{
    int error = switch_bootstrap_validate_root(root);
    if (!error)
        error = switch_migration_wait();
    return error ? error : switch_bootstrap_register_apps(YES);
}

static int switch_migration_recovery(NSString *base, NSString *root, BOOL fullPrepare)
{
    int error = fullPrepare ? switch_bootstrap_prepare_repair(base, root) : switch_bootstrap_readiness(0);
    if (!error && !fullPrepare)
        error = switch_bootstrap_validate_root(root, YES);
    if (!error)
        error = switch_migration_wait();
    return error ? error : switch_bootstrap_register_apps(YES);
}

static int switch_migration_defer_unscheduled(NSString *base, NSString *generation, const char *phase, unsigned attempt)
{
    const int error = switch_migration_write(base, generation, SwitchMigrationFailed, phase, ECANCELED, attempt);
    if (error)
        return error;
    (void)switch_migration_request(base, generation);
    return ECANCELED;
}

static int switch_migration_fail_closed(NSString *base, NSString *root, NSString *generation,
                                        SwitchMigrationStatus previous, NSString *lastPhase, unsigned attempt,
                                        BOOL repair)
{
    switch_bootstrap_readiness(0);
    if (previous == SwitchMigrationWorking) {
        const int error =
            switch_migration_write(base, generation, SwitchMigrationFailed, lastPhase.UTF8String, ECANCELED, attempt);
        if (error)
            return error;
    }
    switch_migration_clear_native(base);
    switch_migration_consume(base);
    return repair ? switch_migration_register_repair(root) : ECANCELED;
}

static int switch_migration_record_failure(NSString *base, NSString *generation, const char *phase, int error,
                                           unsigned attempt, NSArray<NSString *> *loaded)
{
    switch_bootstrap_readiness(0);
    const int saved = switch_migration_write(base, generation, SwitchMigrationFailed, phase, error, attempt);
    if (loaded.count) {
        switch_bootstrap_set_budget(15000);
        switch_bootstrap_unload_daemons(loaded);
    }
    if (!saved) {
        if (error == ETIMEDOUT && switch_migration_timeout_retryable_phase(@(phase)) &&
            attempt < switch_migration_max_attempts) {
            (void)switch_migration_request(base, generation);
        } else {
            switch_migration_clear_native(base);
            switch_migration_consume(base);
        }
    }
    os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: migration %{public}s failed: %d (record %d)", phase, error, saved);
    return saved;
}

int switch_migration_boot(NSString *base, NSString *root, NSString *generation)
{
    int recovery = 1, ready = 0, home = 1;
    int error = switch_bootstrap_read_flag("security.mac.switch_no_tweaks", &recovery);
    if (!error)
        error = switch_bootstrap_read_flag("security.mac.switch_bootstrap_ready", &ready);
    if (!error)
        error = switch_bootstrap_read_flag("kern.switch_migration_home_ready", &home);
    switch_bootstrap_set_budget(switch_migration_command_budget_ms);
    SwitchMigrationStatus previous = SwitchMigrationMissing;
    NSString *lastPhase = nil;
    int recordedError = 0;
    unsigned attempt = 0;
    if (!error)
        error = switch_migration_read(base, generation, &previous, &lastPhase, &recordedError, &attempt);
    if (error) {
        switch_bootstrap_readiness(0);
        return error;
    }
    const BOOL unscheduled =
        previous == SwitchMigrationFailed && recordedError == ECANCELED && [lastPhase isEqualToString:@"unscheduled"];
    const BOOL cancelled = previous == SwitchMigrationFailed && recordedError == ECANCELED;
    const BOOL timedOut = previous == SwitchMigrationFailed && recordedError == ETIMEDOUT;
    const BOOL working = previous == SwitchMigrationWorking;
    const BOOL retryablePhase =
        timedOut ? switch_migration_timeout_retryable_phase(lastPhase) : switch_migration_retryable_phase(lastPhase);
    const BOOL retrying =
        unscheduled || ((working || cancelled || timedOut) && retryablePhase && attempt < switch_migration_max_attempts);
    if (!retrying && (working || previous == SwitchMigrationFailed))
        return switch_migration_fail_closed(base, root, generation, previous, lastPhase, attempt, recovery || !home);
    const BOOL install = previous == SwitchMigrationMissing;
    const BOOL retryFull = retrying && switch_migration_needs_preparation(lastPhase);
    const BOOL resume = retrying && !retryFull;
    const BOOL fullPrepare = install || retryFull;
    const BOOL migrating = install || retrying;
    const unsigned activeAttempt = (retrying && !unscheduled) ? attempt + 1 : 1;
    if (recovery && migrating)
        return switch_migration_recovery(base, root, fullPrepare);
    if (fullPrepare && home) {
        // Only a scheduled migration can start a package transaction; unscheduled deferrals do not burn attempts.
        if (!install)
            switch_bootstrap_readiness(0);
        if (retrying)
            switch_migration_rearm_native_if_interrupted(base);
        const unsigned deferredAttempt = (install || unscheduled) ? 1 : attempt;
        return switch_migration_defer_unscheduled(base, generation, install ? "unscheduled" : lastPhase.UTF8String,
                                                  deferredAttempt);
    }
    if (retrying)
        switch_migration_rearm_native_if_interrupted(base);
    if (migrating)
        switch_migration_mark_native(base);
    const BOOL resumeRuntime = resume && [lastPhase isEqualToString:@"runtime"];
    const BOOL resumeRegistration =
        resume && ([lastPhase isEqualToString:@"registration"] || [lastPhase isEqualToString:@"register-apps"]);
    const BOOL registerApps = (migrating && !resumeRuntime) || !home || recovery;
    const char *resumePhase = resumeRegistration ? "registration" : (lastPhase ? lastPhase.UTF8String : "validation");
    const char *phase = fullPrepare ? "preparation" : resumePhase;
    if (fullPrepare)
        error = switch_migration_write(base, generation, SwitchMigrationWorking, "preparation", 0, activeAttempt);
    else if (resume)
        error = switch_migration_write(base, generation, SwitchMigrationWorking, resumePhase, 0, activeAttempt);
    if (!error && !fullPrepare)
        phase = "validation";
    if (!error)
        error =
            fullPrepare ? switch_bootstrap_prepare_packages(base, root) : switch_bootstrap_validate_root(root, resume);
    if (!error && (!ready || recovery || migrating))
        error = switch_bootstrap_readiness(0);
    if (!error && registerApps) {
        phase = "native-migration";
        if (fullPrepare)
            error = switch_migration_write(base, generation, SwitchMigrationWorking, phase, 0, activeAttempt);
        if (!error)
            error = switch_migration_wait();
        if (!error && migrating)
            switch_migration_clear_native(base);
        if (!error) {
            phase = "register-apps";
            if (migrating && !resumeRuntime && !resumeRegistration)
                error =
                    switch_migration_write(base, generation, SwitchMigrationWorking, "registration", 0, activeAttempt);
        }
        if (!error && !switch_bootstrap_remaining_ms())
            error = ETIMEDOUT;
        else if (!error)
            error = switch_bootstrap_register_apps(recovery != 0);
    }
    NSMutableArray *loaded = [NSMutableArray new];
    if (!error && !recovery && (!ready || migrating)) {
        phase = "runtime";
        if (migrating && !resumeRuntime)
            error = switch_migration_write(base, generation, SwitchMigrationWorking, phase, 0, activeAttempt);
        if (!error)
            error = switch_bootstrap_start_runtime(root, loaded);
        if (!error && !switch_bootstrap_remaining_ms())
            error = ETIMEDOUT;
        if (!error)
            error = switch_bootstrap_readiness(1);
    }
    if (!error) {
        const SwitchMigrationStatus result = recovery ? SwitchMigrationRepair : SwitchMigrationComplete;
        phase = "commit";
        if (previous != result)
            error = switch_migration_write(base, generation, result, NULL, 0);
    }
    if (error) {
        const int saved = switch_migration_record_failure(base, generation, phase, error, activeAttempt, loaded);
        if (!saved && fullPrepare && !strcmp(phase, "preparation")) {
            const int repair = switch_migration_register_repair(root);
            if (repair)
                os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: repair registration failed: %d", repair);
        }
        return error;
    }
    switch_migration_clear_native(base);
    switch_migration_consume(base);
#if SWITCH_DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}@ migration complete, recovery %d", generation, recovery);
#endif
    return 0;
}
