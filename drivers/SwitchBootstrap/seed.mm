// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "seed.h"
#import "state.h"
#include "build_configuration.h"
#include "payload_hash.h"
#include "payload.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static NSArray<NSString *> *switch_seed_held_names(void)
{
    NSArray<NSString *> *every = @[ @"org.coolstar.sileo", @"ellekit", @"uikittools", @"launchctl" ];
    NSDictionary *targets = SWITCH_BOOTSTRAP_MANAGED_PACKAGES;
    NSMutableArray<NSString *> *added = [NSMutableArray new];
    for (NSString *name in targets)
        if (![every containsObject:name])
            [added addObject:name];
    [added sortUsingSelector:@selector(compare:)];
    return [every arrayByAddingObjectsFromArray:added];
}

static int switch_bootstrap_write_firmware(NSString *root)
{
    NSString *path = [root stringByAppendingPathComponent:@"Library/dpkg/status"];
    if (!switch_bootstrap_metadata(path, NO))
        return EPERM;
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text)
        return EIO;
    NSSet *held = [NSSet setWithArray:switch_seed_held_names()];
    NSMutableSet *found = [NSMutableSet new];
    NSMutableArray *records = [NSMutableArray new];
    for (NSString *block in [text componentsSeparatedByString:@"\n\n"]) {
        if (!block.length)
            continue;
        NSMutableArray *lines = [[block componentsSeparatedByString:@"\n"] mutableCopy];
        NSString *name;
        for (NSString *line in lines)
            if ([line hasPrefix:@"Package: "])
                name = [line substringFromIndex:9];
        if (!name || [found containsObject:name])
            return EINVAL;
        [found addObject:name];
        if ([name isEqualToString:@"firmware"])
            continue;
        if ([held containsObject:name]) {
            NSUInteger status = [lines indexOfObjectPassingTest:^BOOL(NSString *line, NSUInteger index, BOOL *stop) {
                (void)index;
                (void)stop;
                return [line hasPrefix:@"Status: "];
            }];
            if (status == NSNotFound || ![lines[status] hasSuffix:@" ok installed"])
                return EINVAL;
            lines[status] = @"Status: hold ok installed";
        }
        while ([lines.lastObject isEqualToString:@""])
            [lines removeLastObject];
        [records addObject:[lines componentsJoinedByString:@"\n"]];
    }
    if (![held isSubsetOfSet:found])
        return EINVAL;
    [records addObject:@"Package: firmware\nStatus: hold ok installed\nPriority: required\nSection: System\n"
                        "Essential: yes\nArchitecture: iphoneos-arm64\nVersion: " SWITCH_BOOTSTRAP_FIRMWARE
                        "\nDescription: iOS firmware version"];
    NSString *updated = [[records componentsJoinedByString:@"\n\n"] stringByAppendingString:@"\n\n"];
    return [updated writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil] ? 0 : EIO;
}

static int switch_bootstrap_firmware(NSString *root)
{
    NSString *path = [root stringByAppendingPathComponent:@"Library/dpkg/lock"];
    const int descriptor = open(path.fileSystemRepresentation, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (descriptor < 0)
        return errno;
    struct flock database_lock = {};
    database_lock.l_type = F_WRLCK;
    database_lock.l_whence = SEEK_SET;
    const int error = fcntl(descriptor, F_SETLK, &database_lock) ? errno : switch_bootstrap_write_firmware(root);
    close(descriptor);
    return error;
}

static NSString *const switch_seed_receipt = @".switch-bootstrap-seed.json";

static NSDictionary *switch_seed_read(NSString *path)
{
    const int file = open(path.fileSystemRepresentation, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0)
        return nil;
    struct stat info;
    NSMutableData *data = [NSMutableData new];
    bool valid = !fstat(file, &info) && S_ISREG(info.st_mode) && !info.st_uid && info.st_nlink == 1 &&
                 !(info.st_mode & 0022) && info.st_size > 0 && info.st_size <= 16384;
    unsigned char bytes[1024];
    while (valid && data.length <= 16384) {
        const ssize_t count = read(file, bytes, sizeof(bytes));
        if (!count)
            break;
        if (count > 0)
            [data appendBytes:bytes length:(NSUInteger)count];
        else if (errno != EINTR)
            valid = false;
    }
    close(file);
    id saved = valid && data.length <= 16384 ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [saved isKindOfClass:NSDictionary.class] ? saved : nil;
}

static BOOL switch_seed_version(NSDictionary *saved)
{
    return [saved[@"schema_version"] isEqual:@1] && [saved[@"build"] isEqual:@SWITCH_BOOTSTRAP_BUILD] &&
           [saved[@"firmware"] isEqual:@SWITCH_BOOTSTRAP_FIRMWARE];
}

static NSDictionary *switch_seed_verified(NSString *root)
{
    if (!switch_bootstrap_metadata(root, YES))
        return nil;
    NSDictionary *saved = switch_seed_read([root stringByAppendingPathComponent:switch_seed_receipt]);
    NSString *hash = saved[@"archive_sha256"];
    if (!switch_seed_version(saved) || ![hash isKindOfClass:NSString.class] || hash.length != 64 ||
        [hash
            rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789abcdef"].invertedSet]
                .location != NSNotFound ||
        !switch_bootstrap_metadata([root stringByAppendingPathComponent:@".procursus_strapped"], NO) ||
        !switch_bootstrap_metadata([root stringByAppendingPathComponent:@".switch-bootstrap-ready"], NO))
        return nil;
    return saved;
}

static int switch_seed_flush(NSString *directory)
{
    const int file = open(directory.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (file < 0)
        return errno;
    const int error = fsync_volume_np(file, SYNC_VOLUME_FULLSYNC | SYNC_VOLUME_WAIT) ? errno : 0;
    close(file);
    return error;
}

static int switch_seed_write(NSString *path, NSDictionary *saved)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:saved options:NSJSONWritingSortedKeys error:nil];
    if (!data || !data.length || data.length > 16384)
        return EINVAL;
    const int file = open(path.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (file < 0)
        return errno;
    size_t offset = 0;
    int error = 0;
    while (!error && offset < data.length) {
        const ssize_t count = write(file, (const char *)data.bytes + offset, data.length - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (!count || errno != EINTR)
            error = count ? errno : EIO;
    }
    if (!error && fsync(file))
        error = errno;
    if (close(file) && !error)
        error = errno;
    if (error)
        unlink(path.fileSystemRepresentation);
    return error;
}

static int switch_seed_publish_state(NSString *root, NSString *base)
{
    NSString *source = [root stringByAppendingPathComponent:switch_seed_receipt];
    NSString *state = [base stringByAppendingPathComponent:@"manifest.json"];
    struct stat info;
    if (!lstat(state.fileSystemRepresentation, &info)) {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
            return EPERM;
    } else if (errno != ENOENT)
        return errno;
    if (rename(source.fileSystemRepresentation, state.fileSystemRepresentation))
        return errno;
    return switch_seed_flush(base);
}

static int switch_seed_restore(NSString *root, NSString *base, NSString *expectedHash)
{
    if (![root isEqual:[base stringByAppendingPathComponent:@"root"]])
        return EINVAL;
    if (!switch_bootstrap_metadata(base, YES))
        return EPERM;
    struct stat info;
    BOOL exists = !lstat(root.fileSystemRepresentation, &info);
    if (!exists || !switch_bootstrap_metadata(root, YES)) {
        if (!exists && errno != ENOENT)
            return errno;
        if (exists) {
            if (!S_ISDIR(info.st_mode) || info.st_uid == 0)
                return EPERM;
            if (!lstat([root stringByAppendingPathComponent:@".switch-bootstrap-activated"].fileSystemRepresentation,
                       &info) ||
                errno != ENOENT)
                return EPERM;
        }
        NSString *temporary = [base stringByAppendingPathComponent:@"root.extracting"];
        NSDictionary *staged = switch_seed_verified(temporary);
        if (![staged[@"archive_sha256"] isEqual:expectedHash])
            return EPERM;
        int error = switch_seed_flush(temporary);
        NSString *old = [base stringByAppendingPathComponent:@"root.unowned"];
        if (!error && exists && renamex_np(root.fileSystemRepresentation, old.fileSystemRepresentation, RENAME_EXCL))
            error = errno;
        if (!error && renamex_np(temporary.fileSystemRepresentation, root.fileSystemRepresentation, RENAME_EXCL))
            error = errno;
        if (error)
            return error;
        error = switch_seed_flush(base);
        if (error)
            return error;
    }
    if (!switch_bootstrap_metadata(root, YES))
        return EPERM;
    NSDictionary *saved = switch_seed_verified(root);
    if (saved)
        return switch_seed_publish_state(root, base);
    if (!lstat([root stringByAppendingPathComponent:switch_seed_receipt].fileSystemRepresentation, &info) ||
        errno != ENOENT)
        return EPERM;
    NSDictionary *existing = switch_seed_read([base stringByAppendingPathComponent:@"manifest.json"]);
    return switch_seed_version(existing) ? 0 : EPERM;
}

int switch_bootstrap_restore_seed(NSString *root, NSString *base)
{
    return switch_seed_restore(root, base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256);
}

int switch_bootstrap_prepare(NSString *root, NSString *base, NSString *archive, NSString *expectedHash)
{
    if (![root isEqual:[base stringByAppendingPathComponent:@"root"]])
        return EINVAL;
    if (!switch_bootstrap_metadata(base, YES))
        return EPERM;
    if (switch_bootstrap_metadata(root, YES))
        return switch_seed_restore(root, base, expectedHash);
    struct stat info;
    BOOL exists = !lstat(root.fileSystemRepresentation, &info);
    if ((exists &&
         (!S_ISDIR(info.st_mode) || info.st_uid == 0 ||
          !lstat([root stringByAppendingPathComponent:@".switch-bootstrap-activated"].fileSystemRepresentation,
                 &info))) ||
        (!exists && errno != ENOENT))
        return EPERM;
    NSString *temporary = [base stringByAppendingPathComponent:@"root.extracting"];
    NSString *old = [base stringByAppendingPathComponent:@"root.unowned"];
    if (exists && !lstat(old.fileSystemRepresentation, &info)) {
        if (!S_ISDIR(info.st_mode) || info.st_uid == 0 || rmdir(old.fileSystemRepresentation))
            return EEXIST;
    } else if (exists && errno != ENOENT)
        return errno;
    NSDictionary *saved = switch_seed_verified(temporary);
    if (![saved[@"archive_sha256"] isEqual:expectedHash]) {
        if (![[switch_bootstrap_digest(archive) lowercaseString] isEqualToString:expectedHash])
            return EAUTH;
        if (!lstat(temporary.fileSystemRepresentation, &info)) {
            if (!switch_bootstrap_metadata(temporary, YES))
                return EPERM;
            if (![NSFileManager.defaultManager removeItemAtPath:temporary error:nil])
                return EIO;
        } else if (errno != ENOENT)
            return errno;
        if (mkdir(temporary.fileSystemRepresentation, 0700))
            return errno;
        int error = switch_bootstrap_extract(archive.fileSystemRepresentation, temporary.fileSystemRepresentation);
        if (!error &&
            (!switch_bootstrap_metadata([temporary stringByAppendingPathComponent:@".procursus_strapped"], NO) ||
             !switch_bootstrap_metadata([temporary stringByAppendingPathComponent:@".switch-bootstrap-ready"], NO)))
            error = EINVAL;
        if (!error)
            error = switch_bootstrap_firmware(temporary);
        if (!error && chmod(temporary.fileSystemRepresentation, 0755))
            error = errno;
        if (!error)
            error = switch_seed_flush(temporary);
        saved = @{
            @"schema_version" : @1,
            @"build" : @SWITCH_BOOTSTRAP_BUILD,
            @"firmware" : @SWITCH_BOOTSTRAP_FIRMWARE,
            @"archive_sha256" : expectedHash,
            @"helper_sha256" : switch_bootstrap_digest(@SWITCH_BOOTSTRAP_HELPER) ?: @"",
            @"holds" : switch_seed_held_names()
        };
        if (!error)
            error = switch_seed_write([temporary stringByAppendingPathComponent:switch_seed_receipt], saved);
        if (error) {
            [NSFileManager.defaultManager removeItemAtPath:temporary error:nil];
            return error;
        }
    }
    int error = switch_seed_flush(temporary);
    if (!error && exists && renamex_np(root.fileSystemRepresentation, old.fileSystemRepresentation, RENAME_EXCL))
        error = errno;
    if (!error && renamex_np(temporary.fileSystemRepresentation, root.fileSystemRepresentation, RENAME_EXCL))
        error = errno;
    if (error)
        return error;
    error = switch_seed_flush(base);
    if (!lstat(old.fileSystemRepresentation, &info) && S_ISDIR(info.st_mode) && info.st_uid != 0)
        (void)rmdir(old.fileSystemRepresentation);
    return error ?: switch_seed_publish_state(root, base);
}
