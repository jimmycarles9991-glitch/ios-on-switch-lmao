// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <cassert>
#include <cstdarg>
#include <cstring>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <os/log.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include "build_configuration.h"
#include "drivers/SwitchBootstrap/core_update.h"

static NSString *switch_test_base;
static NSString *switch_test_lock;
static bool switch_test_deny_parent;
static unsigned switch_test_home;
static unsigned switch_test_migrations;
static unsigned switch_test_readiness;
static int switch_test_migration_error;
static bool switch_test_alias_mode;
static bool switch_test_alias_sync_failure;
static unsigned switch_test_syncs;
static NSString *switch_test_alias;
static bool switch_test_restore_promotion;

static int switch_test_open(const char *path, int flags, ...)
{
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list arguments;
        va_start(arguments, flags);
        mode = (mode_t)va_arg(arguments, int);
        va_end(arguments);
    }
    if (!strcmp(path, "/private/var")) {
        if (switch_test_deny_parent) {
            errno = EACCES;
            return -1;
        }
        return open(switch_test_base.fileSystemRepresentation, flags, mode);
    }
    assert(!strcmp(path, SWITCH_BOOTSTRAP_BASE "/activation.lock"));
    return open(switch_test_lock.fileSystemRepresentation, flags, mode);
}

static int switch_test_fstat(int file, struct stat *info)
{
    const int result = fstat(file, info);
    if (!result)
        info->st_uid = info->st_gid = 0;
    return result;
}

static int switch_test_lstat(const char *path, struct stat *info)
{
    const int result =
        lstat(!strcmp(path, "/private/var/jb") ? switch_test_alias.fileSystemRepresentation : path, info);
    if (!result)
        info->st_uid = info->st_gid = 0;
    return result;
}

static int switch_test_symlink(const char *target, const char *path)
{
    assert(!strcmp(path, "/private/var/jb"));
    return symlink(target, switch_test_alias.fileSystemRepresentation);
}

static ssize_t switch_test_readlink(const char *path, char *value, size_t size)
{
    assert(!strcmp(path, "/private/var/jb"));
    return readlink(switch_test_alias.fileSystemRepresentation, value, size);
}

static int switch_test_lchown(const char *path, uid_t, gid_t)
{
    assert(!strcmp(path, "/private/var/jb"));
    return 0;
}

static int switch_test_sync(int, int flags)
{
    assert(flags == (SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT));
    ++switch_test_syncs;
    if (switch_test_alias_sync_failure) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static int switch_test_chown(int, uid_t user, gid_t group)
{
    assert(!user && !group);
    return 0;
}

static uid_t switch_test_uid(void)
{
    return 0;
}

static int switch_test_executable(char *path, uint32_t *capacity)
{
    assert(*capacity >= sizeof(SWITCH_BOOTSTRAP_HELPER));
    strcpy(path, SWITCH_BOOTSTRAP_HELPER);
    return 0;
}

static char *switch_test_realpath(const char *path, char *canonical)
{
    if (!strcmp(path, "/var/jb"))
        return realpath(switch_test_alias.fileSystemRepresentation, canonical);
    assert(!strcmp(path, SWITCH_BOOTSTRAP_HELPER));
    return strcpy(canonical, path);
}

static int switch_test_sysctl(const char *name, void *, size_t *, void *value, size_t size)
{
    assert(size == sizeof(int));
    if (!strcmp(name, "security.mac.switch_bootstrap_ready")) {
        ++switch_test_readiness;
        return 0;
    }
    assert(!strcmp(name, "kern.switch_migration_home_ready") && *static_cast<int *>(value) == 1);
    const int check = open(switch_test_lock.fileSystemRepresentation, O_RDWR);
    assert(check >= 0 && flock(check, LOCK_EX | LOCK_NB) && errno == EWOULDBLOCK);
    close(check);
    ++switch_test_home;
    return 0;
}

template <class... T> static void switch_test_log(T...)
{}

#define open                 switch_test_open
#define fstat                switch_test_fstat
#define lstat                switch_test_lstat
#define symlink              switch_test_symlink
#define readlink             switch_test_readlink
#define lchown               switch_test_lchown
#define fsync_volume_np      switch_test_sync
#define fchown               switch_test_chown
#define getuid               switch_test_uid
#define geteuid              switch_test_uid
#define realpath             switch_test_realpath
#define sysctlbyname         switch_test_sysctl
#define _NSGetExecutablePath switch_test_executable
#define main                 switch_bootstrap_program_main
#undef os_log_error
#define os_log_error(...) switch_test_log(__VA_ARGS__)
#include "drivers/SwitchBootstrap/bootstrap.mm"
#undef open
#undef fstat
#undef lstat
#undef symlink
#undef readlink
#undef lchown
#undef fsync_volume_np
#undef fchown
#undef getuid
#undef geteuid
#undef realpath
#undef sysctlbyname
#undef _NSGetExecutablePath
#undef main
#undef os_log_error

int switch_migration_boot(NSString *base, NSString *root, NSString *generation)
{
    assert([base isEqual:@SWITCH_BOOTSTRAP_BASE] && [root isEqual:@SWITCH_BOOTSTRAP_ROOT] && generation.length == 64);
    ++switch_test_migrations;
    return switch_test_migration_error;
}

int switch_bootstrap_prepare(NSString *, NSString *, NSString *, NSString *)
{
    assert(false);
    return ENOTSUP;
}

int switch_bootstrap_restore_seed(NSString *root, NSString *)
{
    assert(switch_test_alias_mode);
    if (switch_test_restore_promotion) {
        assert([NSFileManager.defaultManager createDirectoryAtPath:root
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
        switch_test_restore_promotion = false;
    }
    return 0;
}

int switch_bootstrap_update_core(NSString *, NSString *, NSString *, NSString *, NSString *, BOOL, BOOL *)
{
    assert(false);
    return ENOTSUP;
}

int switch_bootstrap_verify_installed_core(NSString *, NSString *)
{
    return 0;
}

int switch_bootstrap_verify_sileo(NSString *)
{
    assert(false);
    return ENOTSUP;
}

int switch_bootstrap_load_daemons(NSString *, NSMutableArray<NSString *> *)
{
    assert(false);
    return ENOTSUP;
}

extern "C" int switch_bootstrap_run(const char *, char *const[])
{
    assert(false);
    return ENOTSUP;
}

extern "C" int switch_bootstrap_run_prep(const char *, char *const[])
{
    assert(false);
    return ENOTSUP;
}

extern "C" int switch_bootstrap_run_update(const char *, char *const[])
{
    assert(false);
    return ENOTSUP;
}

int main()
{
    @autoreleasepool {
        char temporary[] = "/private/tmp/SwitchActivation.XXXXXX";
        assert(mkdtemp(temporary));
        switch_test_base = @(temporary);
        switch_test_lock = [switch_test_base stringByAppendingPathComponent:@"activation.lock"];
        switch_test_alias = [switch_test_base stringByAppendingPathComponent:@"jb"];
        int holder = open(switch_test_lock.fileSystemRepresentation, O_RDWR | O_CREAT | O_EXCL, 0600);
        assert(holder >= 0 && !flock(holder, LOCK_EX | LOCK_NB));
        assert(switch_bootstrap_program_main(1, nullptr) == EX_TEMPFAIL);
        assert(!switch_test_home && !switch_test_migrations && !switch_test_readiness);
        close(holder);
        assert(switch_bootstrap_program_main(1, nullptr) == EX_OK);
        assert(switch_test_home == 1 && switch_test_migrations == 1);
        switch_test_migration_error = EIO;
        assert(switch_bootstrap_program_main(1, nullptr) == EX_SOFTWARE);
        assert(switch_test_home == 2 && switch_test_migrations == 2);
        switch_test_deny_parent = true;
        assert(switch_bootstrap_program_main(1, nullptr) == EX_SOFTWARE);
        assert(switch_test_home == 2 && switch_test_migrations == 2 && !switch_test_readiness);
        switch_test_deny_parent = false;
        assert(!chmod(switch_test_lock.fileSystemRepresentation, 0644));
        assert(switch_bootstrap_program_main(1, nullptr) == EX_SOFTWARE);
        assert(switch_test_home == 2 && switch_test_migrations == 2);
        switch_test_alias_mode = true;
        NSString *root = [switch_test_base stringByAppendingPathComponent:@"root"];
        switch_test_restore_promotion = true;
        switch_test_alias_sync_failure = true;
        assert(switch_bootstrap_validate_root(root) == ENOSPC);
        assert(switch_test_readiness == 1 && switch_test_syncs == 1);
        // Unchanged root and alias skip fsync_volume_np on normal boots.
        assert(!switch_bootstrap_validate_root(root));
        assert(switch_test_syncs == 1);
        switch_test_alias_sync_failure = false;
        assert(!unlink(switch_test_alias.fileSystemRepresentation));
        assert(!switch_bootstrap_validate_root(root));
        assert(switch_test_syncs == 2);
        assert(!switch_bootstrap_validate_root(root));
        assert(switch_test_readiness == 1 && switch_test_syncs == 2);
        assert(!unlink(switch_test_alias.fileSystemRepresentation));
        assert(!symlink("/unrelated", switch_test_alias.fileSystemRepresentation));
        assert(switch_bootstrap_validate_root(root) == EPERM);
        char target[128];
        ssize_t length = readlink(switch_test_alias.fileSystemRepresentation, target, sizeof(target));
        assert(length == 10 && !memcmp(target, "/unrelated", 10));
        assert(!unlink(switch_test_alias.fileSystemRepresentation));
        assert([[NSData data] writeToFile:switch_test_alias atomically:YES]);
        assert(switch_bootstrap_validate_root(root) == EPERM);
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_base error:nil]);
    }
}
