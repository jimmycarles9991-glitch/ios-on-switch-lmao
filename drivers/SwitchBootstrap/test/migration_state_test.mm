// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <initializer_list>
#include <fcntl.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include "build_configuration.h"

static char switch_test_data[] = "/private/tmp/SwitchMigration.XXXXXX";
static ino_t switch_test_wrong_owner;
static ino_t switch_test_other_device;
static bool switch_test_rename_failure;
static bool switch_test_sync_failure;

static int switch_test_open(const char *path, int flags)
{
    assert(!strcmp(path, "/private/var"));
    return open(switch_test_data, flags);
}

static int switch_test_fstat(int fd, struct stat *info)
{
    const int result = fstat(fd, info);
    if (!result) {
        info->st_uid = info->st_ino == switch_test_wrong_owner ? 501 : 0;
        info->st_gid = 0;
        if (info->st_ino == switch_test_other_device)
            ++info->st_dev;
    }
    return result;
}

static int switch_test_rename(int from, const char *name, int to, const char *destination)
{
    if (switch_test_rename_failure) {
        errno = ENOSPC;
        return -1;
    }
    return renameat(from, name, to, destination);
}

static int switch_test_fsync(int fd)
{
    if (switch_test_sync_failure) {
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

#define open     switch_test_open
#define fstat    switch_test_fstat
#define renameat switch_test_rename
#define fsync    switch_test_fsync
#include "drivers/SwitchBootstrap/migration_state.mm"
#undef open
#undef fstat
#undef renameat
#undef fsync

static NSString *const switch_test_base = @SWITCH_BOOTSTRAP_BASE;
static NSString *const switch_test_current = @"924fbf7d251e8ade11b908798987ad41a66e9d23a690b82a3cbf86affb027336";
static NSString *const switch_test_previous = @"6c0596b8ac609191181a91d73deb9aa1960f88ae7026e4277e74f5e4eee20665";

static void switch_test_read(SwitchMigrationStatus expected, int error = 0, NSString *generation = switch_test_current,
                             int line = __builtin_LINE())
{
    SwitchMigrationStatus actual = SwitchMigrationComplete;
    const int result = switch_migration_read(switch_test_base, generation, &actual);
    if (result != error || actual != expected)
        fprintf(stderr, "line %d: read returned %d/status %d, expected %d/status %d\n", line, result, actual, error,
                expected);
    assert(result == error);
    assert(actual == expected);
}

static void switch_test_save(SwitchMigrationStatus value, const char *phase = nullptr, int error = 0)
{
    assert(!switch_migration_write(switch_test_base, switch_test_current, value, phase, error));
    switch_test_read(value);
}

static ino_t switch_test_inode(NSString *path)
{
    struct stat info;
    assert(!lstat(path.fileSystemRepresentation, &info));
    return info.st_ino;
}

int main()
{
    @autoreleasepool {
        assert(mkdtemp(switch_test_data));
        NSFileManager *manager = NSFileManager.defaultManager;
        NSString *parent = [@(switch_test_data) stringByAppendingPathComponent:@"Switch/bootstrap"];
        NSString *base = [parent stringByAppendingPathComponent:@SWITCH_BOOTSTRAP_BUILD];
        NSString *receipt = [base stringByAppendingPathComponent:@"migration.state"];
        NSString *staging = [base stringByAppendingPathComponent:@"migration.state.pending"];
        assert([manager createDirectoryAtPath:base withIntermediateDirectories:YES attributes:nil error:nil]);

        switch_test_read(SwitchMigrationMissing);
        switch_test_save(SwitchMigrationWorking, "registration");
        NSString *interrupted = nil;
        SwitchMigrationStatus previous;
        int recordedError = -1;
        assert(!switch_migration_read(switch_test_base, switch_test_current, &previous, &interrupted, &recordedError));
        assert(previous == SwitchMigrationWorking && [interrupted isEqual:@"registration"] && !recordedError);
        for (int code : {ECANCELED, EIO}) {
            switch_test_save(SwitchMigrationFailed, "registration", code);
            assert(!switch_migration_read(switch_test_base, switch_test_current, &previous, &interrupted, &recordedError));
            assert(previous == SwitchMigrationFailed && [interrupted isEqual:@"registration"] && recordedError == code);
        }
        assert(!switch_migration_read(switch_test_base, switch_test_previous, &previous, &interrupted, &recordedError));
        assert(previous == SwitchMigrationMissing && !interrupted && !recordedError);
        switch_test_save(SwitchMigrationRepair);
        switch_test_save(SwitchMigrationComplete);
        NSData *complete = [NSData dataWithContentsOfFile:receipt];
        assert(([complete isEqualToData:[[NSString stringWithFormat:@"complete %@\n", switch_test_current]
                                            dataUsingEncoding:NSASCIIStringEncoding]]));
        switch_test_read(SwitchMigrationMissing, 0, switch_test_previous);
        switch_test_read(SwitchMigrationMissing, EINVAL, switch_test_current.uppercaseString);
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationMissing, nullptr, 0) ==
               EINVAL);
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationFailed, "bad\nphase", 1) ==
               EINVAL);
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationWorking, nullptr, 0) ==
               EINVAL);
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationFailed, "register", 0) ==
               EINVAL);
        switch_test_read(SwitchMigrationComplete);

        // A power cut before rename can leave both a complete previous generation and a partial staging file.
        assert([@"work" writeToFile:staging atomically:NO encoding:NSASCIIStringEncoding error:nil]);
        switch_test_read(SwitchMigrationComplete);
        switch_test_read(SwitchMigrationMissing, 0, switch_test_previous);
        assert(!switch_migration_write(switch_test_base, switch_test_previous, SwitchMigrationWorking, "prepare", 0));
        switch_test_read(SwitchMigrationMissing);
        switch_test_read(SwitchMigrationWorking, 0, switch_test_previous);
        assert(![manager fileExistsAtPath:staging]);

        for (bool *fault : {&switch_test_rename_failure, &switch_test_sync_failure}) {
            *fault = true;
            assert(switch_migration_write(switch_test_base, switch_test_previous, SwitchMigrationComplete, nullptr,
                                          0) == (fault == &switch_test_rename_failure ? ENOSPC : EIO));
            *fault = false;
            switch_test_read(SwitchMigrationWorking, 0, switch_test_previous);
        }
        switch_test_save(SwitchMigrationComplete);
        for (NSString *bad in @[
                 @"", @"complete", [@"complete " stringByAppendingString:switch_test_current],
                 [NSString stringWithFormat:@"complete %@\n\n", switch_test_current],
                 [NSString stringWithFormat:@"working %@ bad phase\n", switch_test_previous],
                 [NSString stringWithFormat:@"failed %@ register 00\n", switch_test_previous],
                 [NSString stringWithFormat:@"failed %@ register 1000000000000000\n", switch_test_current],
                 [@"" stringByPaddingToLength:161 withString:@"x" startingAtIndex:0]
             ]) {
            assert([bad writeToFile:receipt atomically:NO encoding:NSASCIIStringEncoding error:nil]);
            switch_test_read(SwitchMigrationMissing, EINVAL);
        }
        assert([complete writeToFile:receipt atomically:NO]);
        assert(!link(receipt.fileSystemRepresentation, staging.fileSystemRepresentation));
        switch_test_read(SwitchMigrationMissing, EPERM);
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationComplete, nullptr, 0) ==
               EPERM);
        assert(!unlink(staging.fileSystemRepresentation));
        assert(!symlink(receipt.fileSystemRepresentation, staging.fileSystemRepresentation));
        assert(switch_migration_write(switch_test_base, switch_test_current, SwitchMigrationRepair, nullptr, 0) ==
               ELOOP);
        assert(!unlink(staging.fileSystemRepresentation));
        assert(!rename(receipt.fileSystemRepresentation, staging.fileSystemRepresentation));
        assert(!symlink(staging.fileSystemRepresentation, receipt.fileSystemRepresentation));
        switch_test_read(SwitchMigrationMissing, ELOOP);
        assert(!unlink(receipt.fileSystemRepresentation));
        assert(!rename(staging.fileSystemRepresentation, receipt.fileSystemRepresentation));
        for (NSString *path in @[ receipt, base, parent ]) {
            switch_test_wrong_owner = switch_test_inode(path);
            switch_test_read(SwitchMigrationMissing, EPERM);
            switch_test_wrong_owner = 0;
            switch_test_other_device = switch_test_inode(path);
            switch_test_read(SwitchMigrationMissing, EXDEV);
            switch_test_other_device = 0;
        }
        assert(!chmod(parent.fileSystemRepresentation, 0775));
        switch_test_read(SwitchMigrationMissing, EPERM);
        assert(!chmod(parent.fileSystemRepresentation, 0755));
        switch_test_read(SwitchMigrationComplete);
        acl_t access = acl_from_text("!#acl 1\nuser:FFFFEEEE-DDDD-CCCC-BBBB-AAAA00000000:root:0:allow:read\n");
        assert(access && !acl_set_file(receipt.fileSystemRepresentation, ACL_TYPE_EXTENDED, access));
        acl_free(access);
        switch_test_read(SwitchMigrationMissing, EPERM);
        assert(!unlink(receipt.fileSystemRepresentation));
        assert(!mkfifo(receipt.fileSystemRepresentation, 0600));
        switch_test_read(SwitchMigrationMissing, EPERM);
        assert(!unlink(receipt.fileSystemRepresentation));
        NSString *alternate = [parent stringByAppendingPathComponent:@"elsewhere"];
        assert(!rename(base.fileSystemRepresentation, alternate.fileSystemRepresentation));
        assert(!symlink(alternate.fileSystemRepresentation, base.fileSystemRepresentation));
        SwitchMigrationStatus unsafe = SwitchMigrationComplete;
        assert(switch_migration_read(switch_test_base, switch_test_current, &unsafe));
        assert(unsafe == SwitchMigrationMissing);
        assert([manager removeItemAtPath:@(switch_test_data) error:nil]);
    }
}
