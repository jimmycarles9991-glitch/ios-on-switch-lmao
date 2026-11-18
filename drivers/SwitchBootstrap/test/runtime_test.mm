// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <cassert>
#include <filesystem>
#include <mach-o/dyld.h>
#include <os/log.h>
#include <string>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "archive_fixture.h"
#include "build_configuration.h"

static ino_t switch_unowned_inode;
static int switch_test_disabled;
static bool switch_test_disabled_unavailable;
static int switch_test_update_error;
static unsigned switch_test_captures;
static int switch_test_ready;
static NSMutableArray<NSArray<NSString *> *> *switch_test_updates;
static NSMutableArray<NSString *> *switch_test_lifecycle;
static NSString *switch_test_failure;
static std::vector<std::pair<std::string, int>> switch_test_errors;

static int switch_test_executable(char *path, uint32_t *capacity)
{
    assert(*capacity >= sizeof(SWITCH_BOOTSTRAP_HELPER));
    strcpy(path, SWITCH_BOOTSTRAP_HELPER);
    return 0;
}

static char *switch_test_realpath(const char *path, char *canonical)
{
    if (!strcmp(path, SWITCH_BOOTSTRAP_HELPER))
        return strcpy(canonical, path);
    return realpath(path, canonical);
}

static uid_t switch_test_geteuid()
{
    return 501;
}

static void switch_test_log(const char *message, int error)
{
    switch_test_errors.emplace_back(message, error);
}

static void switch_test_log(const char *, const char *stage, int error)
{
    switch_test_errors.emplace_back(stage, error);
}

static int switch_test_lstat(const char *path, struct stat *info)
{
    const int result = lstat(path, info);
    if (!result)
        info->st_uid = info->st_ino == switch_unowned_inode ? 501 : 0;
    return result;
}

static int switch_test_fstat(int file, struct stat *info)
{
    const int result = fstat(file, info);
    if (!result)
        info->st_uid = info->st_ino == switch_unowned_inode ? 501 : 0;
    return result;
}

static int switch_test_sync(int, int flags)
{
    assert(flags == (SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT));
    return 0;
}

static int switch_test_sysctl(const char *name, void *value, size_t *size, void *newValue, size_t newSize)
{
    if (!strcmp(name, "security.mac.switch_bootstrap_ready")) {
        if (newValue) {
            assert(newSize == sizeof(int));
            switch_test_ready = *static_cast<int *>(newValue);
            [switch_test_lifecycle addObject:switch_test_ready ? @"ready" : @"not ready"];
            ++switch_test_captures;
        } else {
            assert(*size == sizeof(switch_test_ready));
            memcpy(value, &switch_test_ready, *size);
        }
        return 0;
    }
    if (!strcmp(name, "security.mac.switch_no_tweaks")) {
        if (switch_test_disabled_unavailable) {
            errno = EIO;
            return -1;
        }
        assert(*size == sizeof(switch_test_disabled));
        memcpy(value, &switch_test_disabled, *size);
        return 0;
    }
    errno = ENOTSUP;
    return -1;
}

#define lstat                switch_test_lstat
#define fstat                switch_test_fstat
#define fsync_volume_np      switch_test_sync
#define sysctlbyname         switch_test_sysctl
#define main                 switch_bootstrap_program_main
#define _NSGetExecutablePath switch_test_executable
#define realpath             switch_test_realpath
#define geteuid              switch_test_geteuid
#undef os_log_error
#define os_log_error(log, ...) switch_test_log(__VA_ARGS__)
#include "drivers/SwitchBootstrap/bootstrap.mm"
#include "drivers/SwitchBootstrap/core_update.mm"
#include "drivers/SwitchBootstrap/seed.mm"
#undef main
#undef lstat
#undef fstat
#undef fsync_volume_np
#undef sysctlbyname
#undef _NSGetExecutablePath
#undef realpath
#undef geteuid
#undef os_log_error

int switch_migration_boot(NSString *, NSString *, NSString *)
{
    assert(false);
    return ENOTSUP;
}

static int switch_test_record(NSString *command)
{
    [switch_test_lifecycle addObject:command];
    return [switch_test_failure isEqual:command] ? EIO : 0;
}

extern "C" int switch_bootstrap_run(const char *path, char *const args[])
{
    if (!strcmp(path, "/var/jb/usr/bin/launchctl"))
        return switch_test_record(@(args[1]));
    if (!strcmp(path, "/var/jb/bin/sh"))
        return switch_test_record([@(args[1]) lastPathComponent]);
    if (!strcmp(path, "/usr/libexec/switch-appregd")) {
        assert(!strcmp(args[1], "--finish-migration") || !strcmp(args[1], "--register-sileo"));
        return switch_test_record(!strcmp(args[1], "--register-sileo") ? @"repair-register" : @"register");
    }
    assert(false);
    return ENOTSUP;
}

extern "C" int switch_bootstrap_run_prep(const char *path, char *const args[])
{
    assert(!strcmp(path, "/var/jb/bin/sh") && [@(args[1]) hasSuffix:@"/prep_bootstrap.sh"]);
    return switch_test_record(@"prep");
}

extern "C" int switch_bootstrap_run_update(const char *path, char *const arguments[])
{
    assert(!strcmp(path, "/var/jb/usr/bin/dpkg") || !strcmp(path, "/var/jb/usr/bin/apt-mark"));
    NSMutableArray *command = [NSMutableArray new];
    for (unsigned index = 1; arguments[index]; ++index)
        [command addObject:@(arguments[index])];
    [switch_test_updates addObject:command];
    return switch_test_update_error;
}

int switch_bootstrap_load_daemons(NSString *root, NSMutableArray<NSString *> *loaded)
{
    const int error = switch_test_record(@"bootstrap");
    if (!error)
        [loaded addObject:[root stringByAppendingPathComponent:@"Library/LaunchDaemons/job.plist"]];
    return error;
}

void switch_bootstrap_unload_daemons(NSArray<NSString *> *loaded)
{
    if (loaded.count)
        switch_test_record(@"bootout");
}

static void switch_test_check_lifecycle(NSString *root)
{
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *path in @[
             @"etc/rc.d/00-first", @"etc/rc.d/10-second", @"Library/LaunchDaemons/job.plist", @"prep_bootstrap.sh",
             @"usr/lib/ellekit/libinjector.dylib", @"usr/lib/libellekit.dylib", @"usr/lib/ellekit/MobileSafety.dylib"
         ]) {
        NSString *file = [root stringByAppendingPathComponent:path];
        assert([files createDirectoryAtPath:file.stringByDeletingLastPathComponent
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:nil]);
        assert([[NSData data] writeToFile:file atomically:YES]);
    }
    switch_test_lifecycle = [NSMutableArray new];
    switch_test_failure = nil;
    NSMutableArray *loaded = [NSMutableArray new];
    assert(!switch_bootstrap_start_runtime(root, loaded));
    assert(([switch_test_lifecycle isEqual:@[ @"00-first", @"10-second", @"bootstrap" ]]));
    assert(loaded.count == 1);

    [switch_test_lifecycle removeAllObjects];
    [loaded removeAllObjects];
    switch_test_failure = @"00-first";
    assert(switch_bootstrap_start_runtime(root, loaded) == EIO);
    assert(([switch_test_lifecycle isEqual:@[ @"00-first" ]]));
    assert(!loaded.count);

    [switch_test_lifecycle removeAllObjects];
    switch_test_failure = @"bootstrap";
    assert(switch_bootstrap_start_runtime(root, loaded) == EIO);
    assert(([switch_test_lifecycle isEqual:@[ @"00-first", @"10-second", @"bootstrap" ]]));
    assert(!loaded.count);

    [switch_test_lifecycle removeAllObjects];
    switch_test_failure = nil;
    NSString *bundle = [root stringByAppendingPathComponent:@"Applications/Sileo.app"];
    NSString *info = [bundle stringByAppendingPathComponent:@"Info.plist"];
    NSString *executable = [bundle stringByAppendingPathComponent:@"Sileo"];
    assert([files createDirectoryAtPath:bundle withIntermediateDirectories:YES attributes:nil error:nil]);
    NSMutableDictionary *plist =
        [@{@"CFBundleIdentifier" : @"org.coolstar.SileoStore", @"CFBundleExecutable" : @"Sileo"} mutableCopy];
    assert([plist writeToFile:info atomically:YES]);
    assert([@"managed Sileo executable" writeToFile:executable atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    assert(!switch_bootstrap_register_root(root, NO));
    plist[@"CFBundleExecutable"] = @"Other";
    assert([plist writeToFile:info atomically:YES]);
    assert(switch_bootstrap_register_root(root, NO) == EAUTH);
    plist[@"CFBundleExecutable"] = @"Sileo";
    assert([plist writeToFile:info atomically:YES]);
    assert([@"old signed Sileo" writeToFile:executable atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    assert(switch_bootstrap_register_root(root, NO) == EAUTH);
    assert(!switch_bootstrap_register_root(root, YES));
    assert(([switch_test_lifecycle isEqual:@[ @"register", @"repair-register" ]]));

    [switch_test_lifecycle removeAllObjects];
    assert([files removeItemAtPath:[root stringByAppendingPathComponent:@"usr/lib/ellekit/MobileSafety.dylib"]
                             error:nil]);
    assert(switch_bootstrap_start_runtime(root, loaded) == EPERM);
    assert(!switch_test_lifecycle.count && !loaded.count);
    assert(switch_test_errors.back() == std::make_pair(std::string("usr/lib/ellekit/MobileSafety.dylib"), EPERM));
    switch_test_lifecycle = nil;
}

static void switch_bootstrap_check_database_lock(NSString *root)
{
    const std::string path = [[root stringByAppendingPathComponent:@"Library/dpkg/lock"] fileSystemRepresentation];
    int ready[2], finish[2];
    assert(!pipe(ready) && !pipe(finish));
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        close(ready[0]);
        close(finish[1]);
        const int descriptor = open(path.c_str(), O_RDWR | O_CREAT, 0600);
        struct flock lock = {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        assert(descriptor >= 0 && !fcntl(descriptor, F_SETLK, &lock));
        char message = 'L';
        assert(write(ready[1], &message, 1) == 1);
        assert(read(finish[0], &message, 1) == 1);
        close(descriptor);
        _exit(0);
    }
    close(ready[1]);
    close(finish[0]);
    char message = 0;
    assert(read(ready[0], &message, 1) == 1 && message == 'L');
    const int error = switch_bootstrap_firmware(root);
    assert(error == EACCES || error == EAGAIN);
    assert(write(finish[1], &message, 1) == 1);
    close(ready[0]);
    close(finish[1]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    assert(!switch_bootstrap_firmware(root));
}

int main()
{
    @autoreleasepool {
        assert(switch_bootstrap_program_main(1, nullptr) == EX_NOPERM);
        assert(switch_test_errors.empty());
        SwitchBootstrapArchive fixture;
        const std::vector<SwitchArchiveFile> files = {
            {".procursus_strapped", ""},
            {".switch-bootstrap-ready", ""},
            {"Library/dpkg/status", "Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 1\n\n"
                                    "Package: ellekit\nStatus: install ok installed\nVersion: 1\n\n"
                                    "Package: uikittools\nStatus: install ok installed\nVersion: 1\n\n"
                                    "Package: launchctl\nStatus: install ok installed\nVersion: 1:1.2.0+switch1\n"},
            {"usr/share/Switch/bootstrap-packages/sileo.deb", "sileo package"},
            {"usr/share/Switch/bootstrap-packages/ellekit.deb", "ellekit package"},
            {"usr/share/Switch/bootstrap-packages/uikittools.deb", "uikittools package"},
            {"usr/share/Switch/bootstrap-packages/launchctl.deb", "launchctl package"}};
        fixture.write(files);
        NSString *base = @(fixture.root.c_str());
        NSString *root = [base stringByAppendingPathComponent:@"root"];
        NSString *archive = [base stringByAppendingPathComponent:@"payload.tar"];
        NSFileManager *manager = NSFileManager.defaultManager;
        NSString *hash = switch_bootstrap_digest(archive);
        assert(hash.length == 64);
        assert([manager createDirectoryAtPath:root withIntermediateDirectories:NO attributes:nil error:nil]);
        struct stat info;
        assert(!lstat(root.fileSystemRepresentation, &info));
        switch_unowned_inode = info.st_ino;
        NSString *temporary = [base stringByAppendingPathComponent:@"root.extracting"];
        assert([manager createDirectoryAtPath:temporary withIntermediateDirectories:NO attributes:nil error:nil]);
        assert([@"partial" writeToFile:[temporary stringByAppendingPathComponent:@"unfinished"]
                            atomically:YES
                              encoding:NSUTF8StringEncoding
                                 error:nil]);
        assert(!switch_bootstrap_prepare(root, base, archive, hash));
        assert(![manager fileExistsAtPath:temporary]);
        assert(![manager fileExistsAtPath:[base stringByAppendingPathComponent:@"root.unowned"]]);
        NSString *status =
            [NSString stringWithContentsOfFile:[root stringByAppendingPathComponent:@"Library/dpkg/status"]
                                      encoding:NSUTF8StringEncoding
                                         error:nil];
        assert([status containsString:@"Version: " SWITCH_BOOTSTRAP_FIRMWARE]);
        assert([[status componentsSeparatedByString:@"Status: hold ok installed"] count] == 6);
        switch_bootstrap_check_database_lock(root);
        NSString *state = [base stringByAppendingPathComponent:@"manifest.json"];
        NSData *saved = [NSData dataWithContentsOfFile:state];
        assert(saved);
        assert(!switch_bootstrap_activated(root));
        assert(!switch_bootstrap_finish_activation(root));
        assert(switch_bootstrap_activated(root));
        NSString *userPackage = [root stringByAppendingPathComponent:@"user-package"];
        assert([@"keep" writeToFile:userPackage atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        assert([manager removeItemAtPath:state error:nil]);
        assert(switch_bootstrap_prepare(root, base, archive, hash) == EPERM);
        assert([manager fileExistsAtPath:userPackage]);
        assert([saved writeToFile:state atomically:YES]);
        assert([@"corrupt" writeToFile:archive atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        assert(!switch_bootstrap_prepare(root, base, archive, hash));
        assert([manager fileExistsAtPath:userPackage]);
        assert([manager removeItemAtPath:root error:nil]);
        assert([manager removeItemAtPath:archive error:nil]);
        fixture.write(files);
        NSString *old = [base stringByAppendingPathComponent:@"root.unowned"];
        assert([manager createDirectoryAtPath:old withIntermediateDirectories:NO attributes:nil error:nil]);
        assert(!lstat(old.fileSystemRepresentation, &info));
        switch_unowned_inode = info.st_ino;
        assert([manager createDirectoryAtPath:temporary withIntermediateDirectories:NO attributes:nil error:nil]);
        assert(!switch_bootstrap_prepare(root, base, archive, hash));
        assert([manager fileExistsAtPath:root]);
        assert(![manager fileExistsAtPath:old]);
        assert(!switch_bootstrap_finish_activation(root));
        assert([manager removeItemAtPath:[root stringByAppendingPathComponent:@".switch-bootstrap-activated"]
                                   error:nil]);
        assert(!switch_bootstrap_activated(root));
        int disabled = 0;
        switch_test_disabled = 1;
        assert(!switch_bootstrap_read_flag("security.mac.switch_no_tweaks", &disabled) && disabled == 1);
        switch_test_disabled = 0;
        assert(!switch_bootstrap_read_flag("security.mac.switch_no_tweaks", &disabled) && disabled == 0);
        switch_test_disabled_unavailable = true;
        assert(switch_bootstrap_read_flag("security.mac.switch_no_tweaks", &disabled) == EIO);
        switch_test_disabled_unavailable = false;
        assert(!switch_bootstrap_finish_activation(root));
        assert([@"keep" writeToFile:userPackage atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        NSMutableDictionary *previous = [[NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:state]
                                                                         options:0
                                                                           error:nil] mutableCopy];
        previous[@"archive_sha256"] = @"previous build";
        previous[@"uikittools_migration_pending"] = @YES;
        assert([[NSJSONSerialization dataWithJSONObject:previous options:0
                                                  error:nil] writeToFile:state atomically:YES]);
        NSString *helper = [base stringByAppendingPathComponent:@"helper"];
        assert([@"helper" writeToFile:helper atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        NSString *packages = [base stringByAppendingPathComponent:@"packages"];
        assert([manager createDirectoryAtPath:packages withIntermediateDirectories:NO attributes:nil error:nil]);
        for (NSString *name in @[ @"sileo", @"ellekit", @"uikittools", @"launchctl" ]) {
            NSString *path = [packages stringByAppendingPathComponent:[name stringByAppendingString:@".deb"]];
            assert([[name stringByAppendingString:@" package"] writeToFile:path
                                                                atomically:YES
                                                                  encoding:NSUTF8StringEncoding
                                                                     error:nil]);
        }
        switch_test_updates = [NSMutableArray new];
        BOOL updated = YES;
        assert(!switch_bootstrap_update_core(root, base, packages, hash, helper, YES, &updated));
        assert(!updated && !switch_test_updates.count && switch_bootstrap_activated(root));
        switch_test_update_error = EIO;
        assert(switch_bootstrap_update_core(root, base, packages, hash, helper, NO, &updated) == EIO);
        assert(!updated && !switch_bootstrap_core_current(base, hash));
        assert(!switch_bootstrap_activated(root) && [manager fileExistsAtPath:userPackage]);
        assert(switch_test_updates.count == 1 && switch_test_updates[0].count == 7);
        assert([switch_test_updates[0][0] isEqual:@"--force-confold"]);
        assert([switch_test_updates[0][3] hasSuffix:@"/sileo.deb"]);
        assert([switch_test_updates[0][6] hasSuffix:@"/launchctl.deb"]);
        switch_test_update_error = 0;
        switch_test_captures = 0;
        NSString *statusPath = [root stringByAppendingPathComponent:@"Library/dpkg/status"];
        NSData *database = [NSData dataWithContentsOfFile:statusPath];
        assert(!switch_bootstrap_update_core(root, base, packages, hash, helper, NO, &updated));
        assert(updated && switch_test_captures == 2 && switch_bootstrap_core_current(base, hash));
        assert([manager fileExistsAtPath:userPackage]);
        assert(!switch_bootstrap_core_state(base)[@"uikittools_migration_pending"]);
        assert([switch_bootstrap_core_state(base)[@"helper_sha256"] isEqual:switch_bootstrap_digest(helper)]);
        assert([database isEqual:[NSData dataWithContentsOfFile:statusPath]]);
        const NSUInteger commands = switch_test_updates.count;
        assert(!switch_bootstrap_update_core(root, base, packages, hash, helper, NO, &updated));
        assert(!updated && switch_test_updates.count == commands);
        switch_test_check_lifecycle(root);
        assert([manager removeItemAtPath:base error:nil]);
    }
}
