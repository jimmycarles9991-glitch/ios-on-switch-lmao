// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "migration_boot.h"
#import "migration_state.h"
#import "daemon_jobs.h"
#import "state.h"
#include "process.h"

#include <fcntl.h>
#include <os/log.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr unsigned switch_migration_command_budget_ms = 450000;

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

static void switch_migration_consume(NSString *base)
{
    if (unlink([base stringByAppendingPathComponent:@"migration.pending"].fileSystemRepresentation) && errno != ENOENT)
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not clear migration request: %d", errno);
}

static int switch_migration_wait(void)
{
    const int error = switch_wait_for_native_migration("/private/var/datamigrator/migrate");
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
    if (!error)
        error = switch_migration_read(base, generation, &previous, &lastPhase, &recordedError);
    if (error) {
        switch_bootstrap_readiness(0);
        return error;
    }
    // Older workers converted an interrupted registration to a failed registration with ECANCELED.
    const BOOL cancelled = previous == SwitchMigrationFailed && recordedError == ECANCELED;
    const BOOL working = previous == SwitchMigrationWorking;
    const BOOL resume = ((working || cancelled) && [lastPhase isEqualToString:@"registration"]) ||
                        (working && [lastPhase isEqualToString:@"native-migration"]);
    if (!resume && (previous == SwitchMigrationWorking || previous == SwitchMigrationFailed)) {
        switch_bootstrap_readiness(0);
        if (previous == SwitchMigrationWorking) {
            error = switch_migration_write(base, generation, SwitchMigrationFailed, lastPhase.UTF8String, ECANCELED);
            if (error)
                return error;
        }
        switch_migration_consume(base);
        return recovery || !home ? switch_migration_register_repair(root) : ECANCELED;
    }
    const BOOL install = previous == SwitchMigrationMissing;
    const BOOL migrating = install || resume;
    if (recovery && migrating) {
        error = install ? switch_bootstrap_prepare_repair(base, root) : switch_bootstrap_readiness(0);
        if (!error && resume)
            error = switch_bootstrap_validate_root(root, YES);
        if (!error)
            error = switch_migration_wait();
        return error ? error : switch_bootstrap_register_apps(YES);
    }
    const BOOL registerApps = migrating || !home || recovery;
    if (install && home) {
        // Only a scheduled migration can start a package transaction.
        error = switch_migration_write(base, generation, SwitchMigrationFailed, "unscheduled", ECANCELED);
        if (!error)
            switch_migration_consume(base);
        return error ?: ECANCELED;
    }
    const char *phase = resume ? "validation" : "preparation";
    if (install)
        error = switch_migration_write(base, generation, SwitchMigrationWorking, phase, 0);
    if (!error)
        error = install ? switch_bootstrap_prepare_packages(base, root) : switch_bootstrap_validate_root(root, resume);
    if (!error && (!ready || recovery || resume))
        error = switch_bootstrap_readiness(0);
    if (!error && registerApps) {
        phase = "native-migration";
        if (install)
            error = switch_migration_write(base, generation, SwitchMigrationWorking, phase, 0);
        if (!error)
            error = switch_migration_wait();
        if (!error) {
            phase = "register-apps";
            if (migrating)
                error = switch_migration_write(base, generation, SwitchMigrationWorking, "registration", 0);
        }
        if (!error && !switch_bootstrap_remaining_ms())
            error = ETIMEDOUT;
        else if (!error)
            error = switch_bootstrap_register_apps(recovery != 0);
    }
    NSMutableArray *loaded = [NSMutableArray new];
    if (!error && !recovery && (!ready || migrating)) {
        phase = "runtime";
        if (migrating)
            error = switch_migration_write(base, generation, SwitchMigrationWorking, phase, 0);
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
        switch_bootstrap_readiness(0);
        const int saved = switch_migration_write(base, generation, SwitchMigrationFailed, phase, error);
        if (loaded.count) {
            switch_bootstrap_set_budget(15000);
            switch_bootstrap_unload_daemons(loaded);
        }
        if (!saved)
            switch_migration_consume(base);
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: migration %{public}s failed: %d (record %d)", phase, error,
                     saved);
        if (!saved && install && !strcmp(phase, "preparation")) {
            const int repair = switch_migration_register_repair(root);
            if (repair)
                os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: repair registration failed: %d", repair);
        }
        return error;
    }
    switch_migration_consume(base);
#if SWITCH_DEBUG
    os_log(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}@ migration complete, recovery %d", generation, recovery);
#endif
    return 0;
}
