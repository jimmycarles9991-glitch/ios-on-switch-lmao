// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "core_update.h"
#import "state.h"
#include "payload_hash.h"

#include <copyfile.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static NSArray<NSString *> *switch_bootstrap_core_names(void)
{
    return @[ @"org.coolstar.sileo", @"ellekit", @"uikittools", @"launchctl" ];
}

static NSDictionary *switch_bootstrap_core_targets(void)
{
    return SWITCH_BOOTSTRAP_MANAGED_PACKAGES;
}

int switch_bootstrap_verify_sileo(NSString *root)
{
    NSString *applications = [root stringByAppendingPathComponent:@"Applications"];
    NSString *bundle = [applications stringByAppendingPathComponent:@"Sileo.app"];
    NSString *info = [bundle stringByAppendingPathComponent:@"Info.plist"];
    NSString *executable = [bundle stringByAppendingPathComponent:@"Sileo"];
    if (!switch_bootstrap_metadata(root, YES) || !switch_bootstrap_metadata(applications, YES) ||
        !switch_bootstrap_metadata(bundle, YES) || !switch_bootstrap_metadata(info, NO) ||
        !switch_bootstrap_metadata(executable, NO))
        return EPERM;
    struct stat metadata;
    if (lstat(info.fileSystemRepresentation, &metadata))
        return errno;
    if (metadata.st_size <= 0 || metadata.st_size > 1024 * 1024)
        return EFTYPE;
    NSDictionary *plist = [NSDictionary dictionaryWithContentsOfFile:info];
    if (![plist[@"CFBundleExecutable"] isEqual:@"Sileo"] ||
        ![plist[@"CFBundleIdentifier"] isEqual:@"org.coolstar.SileoStore"])
        return EAUTH;
    return [switch_bootstrap_digest(executable) isEqual:@SWITCH_BOOTSTRAP_SILEO_SHA256] ? 0 : EAUTH;
}

static NSDictionary *switch_bootstrap_core_state(NSString *base)
{
    NSString *path = [base stringByAppendingPathComponent:@"manifest.json"];
    if (!switch_bootstrap_metadata(path, NO))
        return nil;
    NSData *data = [NSData dataWithContentsOfFile:path];
    id state = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
    return [state isKindOfClass:NSDictionary.class] ? state : nil;
}

BOOL switch_bootstrap_core_current(NSString *base, NSString *hash)
{
    NSDictionary *state = switch_bootstrap_core_state(base);
    return [state[@"archive_sha256"] isEqual:hash] &&
           [state[@"managed_generation"] isEqual:@SWITCH_BOOTSTRAP_GENERATION] &&
           [state[@"managed_packages"] isEqual:switch_bootstrap_core_targets()];
}

static int switch_bootstrap_save_core_state(NSString *base, NSDictionary *state)
{
    NSData *data = [NSJSONSerialization dataWithJSONObject:state options:NSJSONWritingSortedKeys error:nil];
    NSString *path = [base stringByAppendingPathComponent:@"manifest.json"];
    if (!data || ![data writeToFile:path atomically:YES] || chmod(path.fileSystemRepresentation, 0644))
        return EIO;
    return 0;
}

static NSDictionary<NSString *, NSDictionary *> *switch_bootstrap_core_records(NSString *root)
{
    NSString *path = [root stringByAppendingPathComponent:@"Library/dpkg/status"];
    if (!switch_bootstrap_metadata(path, NO))
        return nil;
    NSString *text = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!text)
        return nil;
    NSMutableDictionary *records = [NSMutableDictionary new];
    NSArray *names = switch_bootstrap_core_names();
    for (NSString *record in [text componentsSeparatedByString:@"\n\n"]) {
        NSMutableDictionary *fields = [NSMutableDictionary new];
        for (NSString *line in [record componentsSeparatedByString:@"\n"]) {
            for (NSString *key in @[ @"Package", @"Version", @"Status", @"Triggers-Pending" ]) {
                NSString *prefix = [key stringByAppendingString:@": "];
                if (![line hasPrefix:prefix])
                    continue;
                if (fields[key])
                    return nil;
                fields[key] = [line substringFromIndex:prefix.length];
            }
        }
        NSString *name = fields[@"Package"];
        if (!name || ![names containsObject:name])
            continue;
        if (records[name])
            return nil;
        records[name] = fields;
    }
    return records;
}

static BOOL switch_bootstrap_core_version(NSDictionary *record, NSDictionary *target, BOOL configured)
{
    if (![record[@"Version"] isEqual:target[@"version"]])
        return NO;
    NSArray *status = [record[@"Status"] componentsSeparatedByString:@" "];
    if (status.count != 3 || ![status[1] isEqual:@"ok"] ||
        (![status[0] isEqual:@"hold"] && ![status[0] isEqual:@"install"]))
        return NO;
    return [status[2] isEqual:@"installed"] ||
           (!configured && ([status[2] isEqual:@"triggers-pending"] || [status[2] isEqual:@"triggers-awaited"]));
}

static NSArray<NSString *> *switch_bootstrap_core_pending(NSDictionary *records)
{
    NSMutableArray *pending = [NSMutableArray new];
    for (NSString *name in switch_bootstrap_core_names())
        if (records[name][@"Triggers-Pending"])
            [pending addObject:name];
    return pending;
}

static BOOL switch_bootstrap_core_held(NSDictionary *records)
{
    for (NSString *name in switch_bootstrap_core_names())
        if (![records[name][@"Status"] isEqual:@"hold ok installed"])
            return NO;
    return YES;
}

int switch_bootstrap_verify_installed_core(NSString *base, NSString *root)
{
    NSString *library = [root stringByAppendingPathComponent:@"Library"];
    if (!switch_bootstrap_metadata(library, YES) ||
        !switch_bootstrap_metadata([library stringByAppendingPathComponent:@"dpkg"], YES))
        return EPERM;
    if (!switch_bootstrap_core_current(base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256))
        return EAUTH;
    NSDictionary *records = switch_bootstrap_core_records(root);
    if (!switch_bootstrap_core_held(records) || switch_bootstrap_core_pending(records).count)
        return EINVAL;
    NSDictionary *targets = switch_bootstrap_core_targets();
    for (NSString *name in targets)
        if (!switch_bootstrap_core_version(records[name], targets[name], YES))
            return EINVAL;
    return 0;
}

static int switch_bootstrap_copy_package(NSString *source, NSString *target, NSDictionary *package)
{
    const int input = open(source.fileSystemRepresentation, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0)
        return errno;
    struct stat info;
    int error = 0;
    if (fstat(input, &info))
        error = errno;
    else if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
             (unsigned long long)info.st_size != [package[@"size"] unsignedLongLongValue])
        error = EAUTH;
    int output = -1;
    if (!error) {
        output = open(target.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (output < 0)
            error = errno;
    }
    if (!error && fcopyfile(input, output, NULL, COPYFILE_DATA))
        error = errno;
    if (output >= 0 && close(output) && !error)
        error = errno;
    close(input);
    if (!error &&
        (!switch_bootstrap_metadata(target, NO) || ![switch_bootstrap_digest(target) isEqual:package[@"sha256"]]))
        error = EAUTH;
    return error;
}

static int switch_bootstrap_stage_core(NSString *source, NSString *candidate, NSArray<NSString *> *selected)
{
    struct stat info;
    NSFileManager *manager = NSFileManager.defaultManager;
    if (!lstat(candidate.fileSystemRepresentation, &info)) {
        if (!switch_bootstrap_metadata(candidate, YES))
            return EPERM;
        if (![manager removeItemAtPath:candidate error:nil])
            return EIO;
    } else if (errno != ENOENT)
        return errno;
    if (mkdir(candidate.fileSystemRepresentation, 0700))
        return errno;
    NSDictionary *targets = switch_bootstrap_core_targets();
    for (NSString *name in selected) {
        NSDictionary *package = targets[name];
        NSString *file = package[@"file"];
        int error = switch_bootstrap_copy_package([source stringByAppendingPathComponent:file],
                                                  [candidate stringByAppendingPathComponent:file], package);
        if (error)
            return error;
    }
    return 0;
}

static int switch_bootstrap_install_core(NSString *root, NSString *candidate, NSArray<NSString *> *selected)
{
    int error = switch_bootstrap_readiness(0);
    NSString *marker = [root stringByAppendingPathComponent:@".switch-bootstrap-activated"];
    if (!error && unlink(marker.fileSystemRepresentation) && errno != ENOENT)
        error = errno;
    if (!error && selected.count) {
        NSDictionary *targets = switch_bootstrap_core_targets();
        NSMutableArray *arguments =
            [NSMutableArray arrayWithArray:@[ @"--force-confold", @"--no-triggers", @"--install" ]];
        for (NSString *name in selected)
            [arguments addObject:[candidate stringByAppendingPathComponent:targets[name][@"file"]]];
        error = switch_bootstrap_update_command(@"/var/jb/usr/bin/dpkg", arguments);
    }
    if (!error)
        error = switch_bootstrap_readiness(0);
    NSDictionary *records = error ? nil : switch_bootstrap_core_records(root);
    if (!error && !records)
        error = EIO;
    NSArray *pending = switch_bootstrap_core_pending(records);
    if (!error && pending.count) {
        NSMutableArray *triggers = [NSMutableArray arrayWithArray:@[ @"--no-triggers", @"--triggers-only" ]];
        [triggers addObjectsFromArray:pending];
        error = switch_bootstrap_update_command(@"/var/jb/usr/bin/dpkg", triggers);
    }
    records = error ? nil : switch_bootstrap_core_records(root);
    if (!error)
        for (NSString *name in switch_bootstrap_core_names())
            if (!switch_bootstrap_core_version(records[name], switch_bootstrap_core_targets()[name], YES))
                return EIO;
    return error ?: switch_bootstrap_update_command(@"/var/jb/usr/bin/apt-mark", @[
        @"hold", @"org.coolstar.sileo", @"ellekit", @"uikittools", @"launchctl"
    ]);
}

int switch_bootstrap_update_core(NSString *root, NSString *base, NSString *packages, NSString *hash, NSString *helper,
                                 BOOL recovery, BOOL *updated)
{
    *updated = NO;
    if (recovery)
        return 0;
    if (!switch_bootstrap_metadata(root, YES) || !switch_bootstrap_metadata(base, YES))
        return EPERM;
    NSMutableDictionary *state = [switch_bootstrap_core_state(base) mutableCopy];
    NSDictionary *records = switch_bootstrap_core_records(root);
    if (!state || !records)
        return EINVAL;
    NSDictionary *targets = switch_bootstrap_core_targets();
    id previous = state[@"managed_packages"];
    BOOL seeded = !previous && [state[@"archive_sha256"] isEqual:hash];
    for (NSString *name in switch_bootstrap_core_names())
        seeded = seeded && switch_bootstrap_core_version(records[name], targets[name], YES);
    NSMutableArray *selected = [NSMutableArray new];
    for (NSString *name in switch_bootstrap_core_names()) {
        BOOL identical = [previous isKindOfClass:NSDictionary.class] && [previous[name] isEqual:targets[name]];
        if (!(identical || seeded) || !switch_bootstrap_core_version(records[name], targets[name], NO))
            [selected addObject:name];
    }
    if (!selected.count && !switch_bootstrap_core_pending(records).count && switch_bootstrap_core_held(records) &&
        switch_bootstrap_core_current(base, hash))
        return 0;
    NSString *helperHash = switch_bootstrap_digest(helper);
    if (!helperHash)
        return EINVAL;
    NSString *candidate = [base stringByAppendingPathComponent:@"core.extracting"];
    int error = selected.count ? switch_bootstrap_stage_core(packages, candidate, selected) : 0;
    if (!error &&
        (selected.count || switch_bootstrap_core_pending(records).count || !switch_bootstrap_core_held(records)))
        error = switch_bootstrap_install_core(root, candidate, selected);
    if (!error) {
        state[@"archive_sha256"] = hash;
        state[@"helper_sha256"] = helperHash;
        state[@"managed_generation"] = @SWITCH_BOOTSTRAP_GENERATION;
        state[@"managed_packages"] = targets;
        state[@"holds"] = switch_bootstrap_core_names();
        [state removeObjectForKey:@"uikittools_migration_pending"];
        error = switch_bootstrap_save_core_state(base, state);
    }
    *updated = !error && selected.count != 0;
    if (selected.count && switch_bootstrap_metadata(candidate, YES) &&
        ![NSFileManager.defaultManager removeItemAtPath:candidate error:nil] && !error)
        error = EIO;
    return error;
}
