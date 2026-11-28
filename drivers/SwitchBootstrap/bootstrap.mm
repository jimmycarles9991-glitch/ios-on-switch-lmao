// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <os/log.h>
#include <stdlib.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sysexits.h>
#include <unistd.h>

#include "build_configuration.h"
#include "payload_hash.h"
#include "payload.h"
#include "process.h"
#import "daemon_jobs.h"
#import "core_update.h"
#import "state.h"
#import "seed.h"
#import "migration_state.h"
#import "migration_boot.h"

static NSString *const switch_bootstrap_source_archive = @"/usr/share/Switch/bootstrap/payload.tar";
static NSString *const switch_bootstrap_root = @SWITCH_BOOTSTRAP_ROOT;
static NSString *const switch_bootstrap_base = @SWITCH_BOOTSTRAP_BASE;

NSString *switch_bootstrap_digest(NSString *path)
{
    const int file = open(path.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0)
        return nil;
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    unsigned char bytes[65536], hash[CC_SHA256_DIGEST_LENGTH];
    ssize_t count;
    while ((count = read(file, bytes, sizeof(bytes))) > 0)
        CC_SHA256_Update(&context, bytes, (CC_LONG)count);
    close(file);
    if (count < 0)
        return nil;
    CC_SHA256_Final(hash, &context);
    NSMutableString *text = [NSMutableString stringWithCapacity:64];
    for (unsigned char byte : hash)
        [text appendFormat:@"%02x", byte];
    return text;
}

BOOL switch_bootstrap_metadata(NSString *path, BOOL directory)
{
    struct stat info;
    return lstat(path.fileSystemRepresentation, &info) == 0 && info.st_uid == 0 && !(info.st_mode & 0022) &&
           (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1);
}

static int switch_bootstrap_adopt_parents(void)
{
    int parent = open("/private/var", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0)
        return errno;
    const char *names[] = {"Switch", "bootstrap", SWITCH_BOOTSTRAP_BUILD};
    for (const char *name : names) {
        if (mkdirat(parent, name, 0755) && errno != EEXIST) {
            const int error = errno;
            close(parent);
            return error;
        }
        const int next = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0) {
            const int error = errno;
            close(parent);
            return error;
        }
        close(parent);
        parent = next;
        if (fchown(parent, 0, 0) || fchmod(parent, 0755)) {
            const int error = errno;
            close(parent);
            return error;
        }
    }
    close(parent);
    return 0;
}

int switch_bootstrap_readiness(int ready)
{
    return sysctlbyname("security.mac.switch_bootstrap_ready", NULL, NULL, &ready, sizeof(ready)) ? errno : 0;
}

int switch_bootstrap_read_flag(const char *name, int *flag)
{
    size_t size = sizeof(*flag);
    if (sysctlbyname(name, flag, &size, NULL, 0))
        return errno;
    return size == sizeof(*flag) ? 0 : EINVAL;
}

static BOOL switch_bootstrap_activated(NSString *root)
{
    NSString *path = [root stringByAppendingPathComponent:@".switch-bootstrap-activated"];
    struct stat info;
    return switch_bootstrap_metadata(path, NO) && !lstat(path.fileSystemRepresentation, &info) && !info.st_size;
}

static int switch_bootstrap_finish_activation(NSString *root)
{
    NSString *marker = [root stringByAppendingPathComponent:@".switch-bootstrap-activated"];
    if (![[NSData data] writeToFile:marker atomically:YES] || chmod(marker.fileSystemRepresentation, 0644))
        return EIO;
    return 0;
}

static int switch_bootstrap_command(NSString *path, NSArray<NSString *> *arguments,
                                    int (*run)(const char *, char *const[]) = switch_bootstrap_run)
{
    // A development image updates up to 9 managed packages in one dpkg call (3 flags + 9 debs).
    char *argv[32];
    if (arguments.count > sizeof(argv) / sizeof(argv[0]) - 2)
        return E2BIG;
    argv[0] = (char *)path.fileSystemRepresentation;
    for (NSUInteger index = 0; index < arguments.count; ++index)
        argv[index + 1] = (char *)arguments[index].fileSystemRepresentation;
    argv[arguments.count + 1] = NULL;
    return run(path.fileSystemRepresentation, argv);
}

int switch_bootstrap_update_command(NSString *path, NSArray<NSString *> *arguments)
{
    return switch_bootstrap_command(path, arguments, switch_bootstrap_run_update);
}

static int switch_bootstrap_stage_error(const char *stage, int error)
{
    if (error)
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: %{public}s failed: %d", stage, error);
    return error;
}

int switch_bootstrap_start_runtime(NSString *root, NSMutableArray *loaded)
{
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *library in @[
             @"usr/lib/ellekit/libinjector.dylib", @"usr/lib/libellekit.dylib", @"usr/lib/ellekit/MobileSafety.dylib"
         ]) {
        if (!switch_bootstrap_metadata([root stringByAppendingPathComponent:library], NO))
            return switch_bootstrap_stage_error(library.fileSystemRepresentation, EPERM);
    }

    NSString *scripts = [root stringByAppendingPathComponent:@"etc/rc.d"];
    NSArray *names = [[files contentsOfDirectoryAtPath:scripts error:nil] sortedArrayUsingSelector:@selector(compare:)];
    for (NSString *name in names) {
        NSString *path = [scripts stringByAppendingPathComponent:name];
        if (switch_bootstrap_metadata(path, NO)) {
            const int error = switch_bootstrap_command(@"/var/jb/bin/sh", @[ path ]);
            if (error)
                return switch_bootstrap_stage_error("package startup scripts", error);
        }
    }

    return switch_bootstrap_stage_error("package launch daemons", switch_bootstrap_load_daemons(root, loaded));
}

static int switch_bootstrap_register_root(NSString *root, BOOL repair)
{
    if (!repair) {
        int error = switch_bootstrap_verify_sileo(root);
        if (error)
            return switch_bootstrap_stage_error("Sileo executable identity", error);
    }
    return switch_bootstrap_command(@"/usr/libexec/switch-appregd",
                                    @[ repair ? @"--register-sileo" : @"--finish-migration" ]);
}

int switch_bootstrap_register_apps(BOOL repair)
{
    return switch_bootstrap_register_root(switch_bootstrap_root, repair);
}

int switch_bootstrap_validate_root(NSString *root, BOOL prepared)
{
    struct stat previous, current;
    const BOOL captured = !lstat(root.fileSystemRepresentation, &previous) && S_ISDIR(previous.st_mode) &&
                          previous.st_uid == 0 && !(previous.st_mode & 0022);
    if (prepared && (!switch_bootstrap_metadata(root, YES) || !switch_bootstrap_activated(root)))
        return EPERM;
    NSString *base = root.stringByDeletingLastPathComponent;
    int error = switch_bootstrap_restore_seed(root, base);
    if (error)
        return error;
    if (prepared && (error = switch_bootstrap_verify_installed_core(base, root)))
        return error;
    if (lstat(root.fileSystemRepresentation, &current))
        return errno;
    BOOL didModify = NO;
    if (!captured || previous.st_dev != current.st_dev || previous.st_ino != current.st_ino) {
        didModify = YES;
        if ((error = switch_bootstrap_readiness(0)))
            return error;
    }
    const char *alias = "/private/var/jb";
    struct stat info;
    if (lstat(alias, &info)) {
        if (errno != ENOENT)
            return errno;
        if (symlink(root.fileSystemRepresentation, alias)) {
            if (errno != EEXIST)
                return errno;
        } else {
            didModify = YES;
        }
        if (lstat(alias, &info))
            return errno;
    }
    char target[PATH_MAX];
    const ssize_t length = readlink(alias, target, sizeof(target));
    if (!S_ISLNK(info.st_mode) || length <= 0 || (size_t)length >= sizeof(target))
        return EPERM;
    target[length] = '\0';
    if (strcmp(target, root.fileSystemRepresentation))
        return EPERM;
    char linked[PATH_MAX];
    if (!realpath("/var/jb", linked) || strcmp(linked, root.fileSystemRepresentation))
        return EPERM;
    if (info.st_uid || info.st_gid) {
        if (lchown(alias, 0, 0))
            return errno;
        didModify = YES;
    }
    if (!didModify)
        return 0;
    const int parent = open("/private/var", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent < 0)
        return errno;
    error = fsync_volume_np(parent, SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT) ? errno : 0;
    close(parent);
    return error;
}

int switch_bootstrap_prepare_repair(NSString *base, NSString *root)
{
    int error = switch_bootstrap_prepare(root, base, switch_bootstrap_source_archive, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256);
    if (!error)
        error = switch_bootstrap_validate_root(root);
    if (!error)
        error = switch_bootstrap_readiness(0);
    return error;
}

int switch_bootstrap_prepare_packages(NSString *base, NSString *root)
{
    int error = switch_bootstrap_prepare_repair(base, root);
    BOOL updated = NO;
    if (!error)
        error = switch_bootstrap_update_core(root, base, @"/usr/share/Switch/bootstrap/packages",
                                             @SWITCH_BOOTSTRAP_ARCHIVE_SHA256, @SWITCH_BOOTSTRAP_HELPER, NO, &updated);
    NSString *script = [root stringByAppendingPathComponent:@"prep_bootstrap.sh"];
    if (!error && !switch_bootstrap_activated(root) && [NSFileManager.defaultManager fileExistsAtPath:script])
        error = switch_bootstrap_command(@"/var/jb/bin/sh", @[ script ], switch_bootstrap_run_prep);
    if (!error)
        error = switch_bootstrap_finish_activation(root);
    return error;
}

static int switch_bootstrap_activate(void)
{
    int error = switch_bootstrap_adopt_parents();
    if (error)
        return error;
    NSString *lockPath = [switch_bootstrap_base stringByAppendingPathComponent:@"activation.lock"];
    const int lock =
        open(lockPath.fileSystemRepresentation, O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock < 0)
        return errno;
    struct stat info;
    if (fstat(lock, &info)) {
        error = errno;
        close(lock);
        return error;
    }
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid || (info.st_mode & 0077)) {
        close(lock);
        return EPERM;
    }
    if (flock(lock, LOCK_EX | LOCK_NB)) {
        error = errno;
        close(lock);
        return error == EAGAIN ? EWOULDBLOCK : error;
    }
    error = switch_migration_boot(switch_bootstrap_base, switch_bootstrap_root, @SWITCH_BOOTSTRAP_GENERATION);
    int done = 1;
    if (sysctlbyname("kern.switch_migration_home_ready", NULL, NULL, &done, sizeof(done))) {
        const int releaseError = errno;
        os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: could not close migration window: %d", releaseError);
        if (!error)
            error = releaseError;
    }
    close(lock);
    return error;
}

int main(int argc, char **argv)
{
    (void)argv;
    @autoreleasepool {
        char executable[PATH_MAX], canonical[PATH_MAX];
        uint32_t capacity = sizeof(executable);
        if (_NSGetExecutablePath(executable, &capacity) || !realpath(executable, canonical) ||
            strcmp(canonical, SWITCH_BOOTSTRAP_HELPER))
            return EX_NOPERM;
        if (getuid() != 0 || geteuid() != 0)
            return EX_NOPERM;
        if (argc != 1)
            return EX_USAGE;
        const int error = switch_bootstrap_activate();
        if (error)
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: activation failed: %d", error);
        return error == EWOULDBLOCK ? EX_TEMPFAIL : error ? EX_SOFTWARE : EX_OK;
    }
}
