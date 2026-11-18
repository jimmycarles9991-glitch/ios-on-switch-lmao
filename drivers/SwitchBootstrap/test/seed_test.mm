// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>

#include <cassert>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

#include "drivers/SwitchBootstrap/test/archive_fixture.h"
#include "payload_hash.h"

static std::set<ino_t> switch_test_unowned;
static bool switch_test_fail_write;
static bool switch_test_fail_promote;
static bool switch_test_fail_backup;
static bool switch_test_fail_sync;
static bool switch_test_promoted;
static unsigned switch_test_syncs;
static NSString *switch_test_root;

static int switch_test_lstat(const char *path, struct stat *info)
{
    const int result = lstat(path, info);
    if (!result)
        info->st_uid = switch_test_unowned.count(info->st_ino) ? 501 : 0;
    return result;
}

static int switch_test_fstat(int file, struct stat *info)
{
    const int result = fstat(file, info);
    if (!result)
        info->st_uid = switch_test_unowned.count(info->st_ino) ? 501 : 0;
    return result;
}

static int switch_test_sync(int, int flags)
{
    assert(flags == (SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT));
    ++switch_test_syncs;
    if (switch_test_fail_sync && switch_test_promoted) {
        errno = ENOSPC;
        return -1;
    }
    return 0;
}

static ssize_t switch_test_write(int file, const void *bytes, size_t size)
{
    char path[PATH_MAX];
    assert(!fcntl(file, F_GETPATH, path));
    if (strstr(path, ".switch-bootstrap-seed.json"))
        assert(switch_test_syncs);
    return write(file, bytes, size);
}

static int switch_test_manifest_rename(const char *from, const char *to)
{
    if (switch_test_fail_write) {
        errno = ENOSPC;
        return -1;
    }
    return rename(from, to);
}

static int switch_test_rename(const char *from, const char *to, unsigned flags)
{
    assert(flags == RENAME_EXCL);
    const bool publish = !strcmp(to, switch_test_root.fileSystemRepresentation);
    const bool backup = !strcmp(from, switch_test_root.fileSystemRepresentation);
    if ((publish && switch_test_fail_promote) || (backup && switch_test_fail_backup)) {
        errno = ENOSPC;
        return -1;
    }
    const int result = renamex_np(from, to, flags);
    if (!result && publish)
        switch_test_promoted = true;
    return result;
}

#define lstat           switch_test_lstat
#define fstat           switch_test_fstat
#define fsync_volume_np switch_test_sync
#define write           switch_test_write
#define rename          switch_test_manifest_rename
#define renamex_np      switch_test_rename
#include "drivers/SwitchBootstrap/seed.mm"
#undef lstat
#undef fstat
#undef fsync_volume_np
#undef write
#undef rename
#undef renamex_np

BOOL switch_bootstrap_metadata(NSString *path, BOOL directory)
{
    struct stat info;
    return !switch_test_lstat(path.fileSystemRepresentation, &info) && !info.st_uid && !(info.st_mode & 0022) &&
           (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1);
}

NSString *switch_bootstrap_digest(NSString *path)
{
    NSData *data = [NSData dataWithContentsOfFile:path];
    if (!data)
        return nil;
    unsigned char hash[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes, (CC_LONG)data.length, hash);
    NSMutableString *result = [NSMutableString new];
    for (unsigned char byte : hash)
        [result appendFormat:@"%02x", byte];
    return result;
}

static void switch_test_save(NSString *path, NSString *value)
{
    assert([value writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
}

static void switch_test_prepare(unsigned failure)
{
    switch_test_unowned.clear();
    SwitchBootstrapArchive fixture;
    fixture.write(
        {{".procursus_strapped", ""},
         {".switch-bootstrap-ready", ""},
         {"Library/dpkg/status", "Package: org.coolstar.sileo\nStatus: install ok installed\nVersion: 1\n\n"
                                 "Package: ellekit\nStatus: install ok installed\nVersion: 1\n\n"
                                 "Package: uikittools\nStatus: install ok installed\nVersion: 1\n\n"
                                 "Package: launchctl\nStatus: install ok installed\nVersion: 1:1.2.0+switch1\n\n"}});
    NSString *base = @(fixture.root.c_str());
    switch_test_root = [base stringByAppendingPathComponent:@"root"];
    NSString *archive = [base stringByAppendingPathComponent:@"payload.tar"];
    NSString *hash = switch_bootstrap_digest(archive);
    NSString *state = [base stringByAppendingPathComponent:@"manifest.json"];
    NSString *temporary = [base stringByAppendingPathComponent:@"root.extracting"];
    NSString *backup = [base stringByAppendingPathComponent:@"root.unowned"];
    NSString *oldUser = [backup stringByAppendingPathComponent:@"user-package"];
    NSFileManager *files = NSFileManager.defaultManager;
    switch_test_fail_write = failure == 1;
    switch_test_fail_promote = failure == 2;
    switch_test_fail_backup = failure == 4;
    switch_test_fail_sync = failure == 3;
    switch_test_promoted = false;
    switch_test_syncs = 0;
    if (failure == 2 || failure == 4) {
        assert([files createDirectoryAtPath:switch_test_root withIntermediateDirectories:NO attributes:nil error:nil]);
        struct stat info;
        assert(!lstat(switch_test_root.fileSystemRepresentation, &info));
        switch_test_unowned.insert(info.st_ino);
        switch_test_save([switch_test_root stringByAppendingPathComponent:@"user-package"], @"old data");
    }
    assert(switch_bootstrap_prepare(switch_test_root, base, archive, hash) == ENOSPC);
    assert(![files fileExistsAtPath:state]);
    assert([files removeItemAtPath:archive error:nil]);
    switch_test_fail_write = switch_test_fail_promote = switch_test_fail_backup = switch_test_fail_sync = false;
    if (failure == 4) {
        assert([files fileExistsAtPath:switch_test_root] && [files fileExistsAtPath:temporary]);
        NSDictionary *stale = @{
            @"schema_version" : @1,
            @"build" : @SWITCH_BOOTSTRAP_BUILD,
            @"firmware" : @SWITCH_BOOTSTRAP_FIRMWARE,
            @"archive_sha256" : @"previous"
        };
        assert([[NSJSONSerialization dataWithJSONObject:stale options:0 error:nil] writeToFile:state atomically:YES]);
        assert(!symlink(switch_test_root.fileSystemRepresentation, backup.fileSystemRepresentation));
        assert(switch_seed_restore(switch_test_root, base, hash) == EEXIST);
        assert([files fileExistsAtPath:[switch_test_root stringByAppendingPathComponent:@"user-package"]]);
        assert(!unlink(backup.fileSystemRepresentation));
        assert(!switch_seed_restore(switch_test_root, base, hash));
        assert([files fileExistsAtPath:oldUser]);
        assert([switch_seed_read(state)[@"archive_sha256"] isEqual:hash]);
    } else if (failure == 2) {
        assert(![files fileExistsAtPath:switch_test_root] && [files fileExistsAtPath:temporary]);
        assert([[NSString stringWithContentsOfFile:oldUser encoding:NSUTF8StringEncoding
                                             error:nil] isEqual:@"old data"]);
        assert([files createDirectoryAtPath:switch_test_root withIntermediateDirectories:NO attributes:nil error:nil]);
        NSString *existing = [switch_test_root stringByAppendingPathComponent:@"existing-package"];
        switch_test_save(existing, @"initialized");
        assert(switch_seed_restore(switch_test_root, base, hash) == EPERM);
        assert([files fileExistsAtPath:existing] && [files fileExistsAtPath:temporary]);
        assert([files removeItemAtPath:switch_test_root error:nil]);
        assert(!switch_seed_restore(switch_test_root, base, hash));
        assert(![files fileExistsAtPath:temporary]);
        assert([files fileExistsAtPath:oldUser]);
    } else {
        assert([files fileExistsAtPath:switch_test_root]);
        NSString *user = [switch_test_root stringByAppendingPathComponent:@"new-user-package"];
        switch_test_save(user, @"keep");
        assert(!switch_bootstrap_restore_seed(switch_test_root, base));
        assert([[NSString stringWithContentsOfFile:user encoding:NSUTF8StringEncoding error:nil] isEqual:@"keep"]);
    }
    assert(switch_seed_version(switch_seed_read(state)));
    NSString *seed = [switch_test_root stringByAppendingPathComponent:@".switch-bootstrap-seed.json"];
    assert(![files fileExistsAtPath:seed]);
    assert(!switch_bootstrap_prepare(switch_test_root, base, archive, hash));
    assert([files removeItemAtPath:state error:nil]);
    assert(switch_bootstrap_prepare(switch_test_root, base, archive, hash) == EPERM);
    assert([files fileExistsAtPath:switch_test_root]);
}

static void switch_test_no_replacement(void)
{
    switch_test_unowned.clear();
    SwitchBootstrapArchive fixture;
    NSString *base = @(fixture.root.c_str());
    switch_test_root = [base stringByAppendingPathComponent:@"root"];
    NSString *temporary = [base stringByAppendingPathComponent:@"root.extracting"];
    NSFileManager *files = NSFileManager.defaultManager;
    assert([files createDirectoryAtPath:switch_test_root withIntermediateDirectories:NO attributes:nil error:nil]);
    assert([files createDirectoryAtPath:temporary withIntermediateDirectories:NO attributes:nil error:nil]);
    NSString *user = [switch_test_root stringByAppendingPathComponent:@"user-package"];
    switch_test_save(user, @"preserve initialized data");
    switch_test_save([temporary stringByAppendingPathComponent:@".switch-bootstrap-seed.json"], @"{}");
    assert(switch_bootstrap_restore_seed(switch_test_root, base) == EPERM);
    assert([[NSString stringWithContentsOfFile:user encoding:NSUTF8StringEncoding
                                         error:nil] isEqual:@"preserve initialized data"]);
    assert([files fileExistsAtPath:temporary]);
    assert([files removeItemAtPath:switch_test_root error:nil]);
    assert(switch_seed_restore(switch_test_root, base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256) == EPERM);
    assert(![files fileExistsAtPath:switch_test_root] && [files fileExistsAtPath:temporary]);
}

int main()
{
    @autoreleasepool {
        for (unsigned failure = 1; failure != 5; ++failure)
            switch_test_prepare(failure);
        switch_test_no_replacement();
    }
}
