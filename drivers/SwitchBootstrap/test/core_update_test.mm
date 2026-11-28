// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#include <CommonCrypto/CommonDigest.h>

#include <cassert>
#include <cerrno>
#include <sys/stat.h>
#include <unistd.h>

#import "drivers/SwitchBootstrap/core_update.mm"

static NSString *switch_test_base;
static NSString *switch_test_root;
static NSString *switch_test_sources;
static NSString *switch_test_helper;
static NSMutableArray<NSArray<NSString *> *> *switch_test_commands;
static NSMutableDictionary<NSString *, NSMutableDictionary *> *switch_test_installed;
static NSString *switch_test_failure_action;
static NSString *const switch_test_user = @"Package: user-tweak\nStatus: hold ok triggers-pending\n"
                                           "Triggers-Pending: /var/jb/usr/lib\nVersion: 42\n\n";

BOOL switch_bootstrap_metadata(NSString *path, BOOL directory)
{
    struct stat info;
    return !lstat(path.fileSystemRepresentation, &info) && info.st_uid == getuid() && !(info.st_mode & 0022) &&
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

int switch_bootstrap_readiness(int ready)
{
    assert(!ready);
    return 0;
}

static NSString *switch_test_status(void)
{
    return [switch_test_root stringByAppendingPathComponent:@"Library/dpkg/status"];
}

static void switch_test_write(NSString *path, NSString *text)
{
    assert([text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]);
    assert(!chmod(path.fileSystemRepresentation, 0644));
}

static NSString *switch_test_read(NSString *path)
{
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

static void switch_test_save_status(void)
{
    NSMutableString *status = [NSMutableString new];
    for (NSString *name in switch_bootstrap_core_names()) {
        NSDictionary *record = switch_test_installed[name];
        [status appendFormat:@"Package: %@\nStatus: %@\nVersion: %@\n", name, record[@"status"], record[@"version"]];
        if (record[@"pending"])
            [status appendString:@"Triggers-Pending: /var/jb/Applications\n"];
        if (record[@"awaited"])
            [status appendString:@"Triggers-Awaited: uikittools\n"];
        [status appendString:@"\n"];
    }
    [status appendString:switch_test_user];
    switch_test_write(switch_test_status(), status);
}

int switch_bootstrap_update_command(NSString *path, NSArray<NSString *> *arguments)
{
    [switch_test_commands addObject:[@[ path ] arrayByAddingObjectsFromArray:arguments]];
    if ([arguments containsObject:switch_test_failure_action ?: @""])
        return EIO;
    if ([arguments containsObject:@"--install"]) {
        assert([path isEqual:@"/var/jb/usr/bin/dpkg"]);
        assert([arguments[0] isEqual:@"--force-confold"] && [arguments[1] isEqual:@"--no-triggers"] &&
               [arguments[2] isEqual:@"--install"]);
        BOOL application = NO;
        for (NSString *file in [arguments subarrayWithRange:NSMakeRange(3, arguments.count - 3)]) {
            NSString *name = nil;
            for (NSString *candidate in switch_bootstrap_core_names())
                if ([file.lastPathComponent isEqual:switch_bootstrap_core_targets()[candidate][@"file"]])
                    name = candidate;
            assert(name && switch_bootstrap_metadata(file, NO));
            assert([switch_bootstrap_digest(file) isEqual:switch_bootstrap_core_targets()[name][@"sha256"]]);
            switch_test_installed[name][@"version"] = switch_bootstrap_core_targets()[name][@"version"];
            switch_test_installed[name][@"status"] = @"install ok installed";
            application |= [name isEqual:@"org.coolstar.sileo"] || [name isEqual:@"uikittools"];
        }
        if (application) {
            switch_test_installed[@"org.coolstar.sileo"][@"status"] = @"install ok triggers-awaited";
            switch_test_installed[@"org.coolstar.sileo"][@"awaited"] = @YES;
            switch_test_installed[@"uikittools"][@"status"] = @"install ok triggers-pending";
            switch_test_installed[@"uikittools"][@"pending"] = @YES;
        }
    } else if ([arguments containsObject:@"--triggers-only"]) {
        assert([path isEqual:@"/var/jb/usr/bin/dpkg"]);
        assert(([arguments isEqual:@[ @"--no-triggers", @"--triggers-only", @"uikittools" ]]));
        for (NSString *name in switch_bootstrap_core_names()) {
            switch_test_installed[name][@"status"] = @"install ok installed";
            [switch_test_installed[name] removeObjectForKey:@"pending"];
            [switch_test_installed[name] removeObjectForKey:@"awaited"];
        }
    } else {
        assert([path isEqual:@"/var/jb/usr/bin/apt-mark"]);
        NSArray<NSString *> *names = switch_bootstrap_core_names();
        assert(names.count == switch_bootstrap_core_targets().count);
        assert(([[names subarrayWithRange:NSMakeRange(0, 4)]
            isEqual:@[ @"org.coolstar.sileo", @"ellekit", @"uikittools", @"launchctl" ]]));
        assert(([arguments isEqual:[@[ @"hold" ] arrayByAddingObjectsFromArray:names]]));
        for (NSString *name in switch_bootstrap_core_names())
            switch_test_installed[name][@"status"] = @"hold ok installed";
    }
    switch_test_save_status();
    return 0;
}

static void switch_test_state(BOOL current)
{
    NSMutableDictionary *state =
        [@{@"archive_sha256" : @"previous", @"firmware" : @"17.7.11", @"custom" : @"keep"} mutableCopy];
    if (current) {
        state[@"archive_sha256"] = @SWITCH_BOOTSTRAP_ARCHIVE_SHA256;
        state[@"managed_generation"] = @SWITCH_BOOTSTRAP_GENERATION;
        state[@"managed_packages"] = switch_bootstrap_core_targets();
    }
    NSString *path = [switch_test_base stringByAppendingPathComponent:@"manifest.json"];
    NSData *json = [NSJSONSerialization dataWithJSONObject:state options:0 error:nil];
    assert([json writeToFile:path atomically:YES]);
    assert(!chmod(path.fileSystemRepresentation, 0644));
}

static void switch_test_reset(BOOL current)
{
    NSFileManager *files = NSFileManager.defaultManager;
    for (NSString *path in @[
             switch_test_root, switch_test_sources, [switch_test_base stringByAppendingPathComponent:@"core.extracting"]
         ])
        if ([files fileExistsAtPath:path])
            assert([files removeItemAtPath:path error:nil]);
    assert([files createDirectoryAtPath:[switch_test_root stringByAppendingPathComponent:@"Library/dpkg"]
            withIntermediateDirectories:YES
                             attributes:nil
                                  error:nil]);
    assert([files createDirectoryAtPath:switch_test_sources withIntermediateDirectories:NO attributes:nil error:nil]);
    switch_test_commands = [NSMutableArray new];
    switch_test_installed = [NSMutableDictionary new];
    switch_test_failure_action = nil;
    for (NSString *name in switch_bootstrap_core_names()) {
        NSDictionary *target = switch_bootstrap_core_targets()[name];
        NSString *file = target[@"file"];
        switch_test_write([switch_test_sources stringByAppendingPathComponent:file],
                          [file.stringByDeletingPathExtension stringByAppendingString:@" package"]);
        switch_test_installed[name] =
            [@{@"version" : target[@"version"], @"status" : @"hold ok installed"} mutableCopy];
    }
    switch_test_save_status();
    switch_test_write([switch_test_root stringByAppendingPathComponent:@"user.sources"], @"user repository");
    switch_test_write([switch_test_root stringByAppendingPathComponent:@".switch-bootstrap-activated"], @"");
    switch_test_write(switch_test_helper, @"helper");
    switch_test_state(current);
}

static int switch_test_update(BOOL recovery, BOOL *updated)
{
    int error = switch_bootstrap_update_core(switch_test_root, switch_test_base, switch_test_sources,
                                             @SWITCH_BOOTSTRAP_ARCHIVE_SHA256, switch_test_helper, recovery, updated);
    assert([switch_test_read(switch_test_status()) containsString:switch_test_user]);
    assert([switch_test_read([switch_test_root stringByAppendingPathComponent:@"user.sources"])
        isEqual:@"user repository"]);
    return error;
}

static void switch_test_delta(void)
{
    switch_test_reset(YES);
    NSMutableDictionary *state = [switch_bootstrap_core_state(switch_test_base) mutableCopy];
    NSMutableDictionary *records = [state[@"managed_packages"] mutableCopy];
    NSMutableDictionary *old = [records[@"org.coolstar.sileo"] mutableCopy];
    old[@"sha256"] = @"old";
    records[@"org.coolstar.sileo"] = old;
    state[@"managed_packages"] = records;
    assert(!switch_bootstrap_save_core_state(switch_test_base, state));
    for (NSString *name in @[ @"ellekit.deb", @"uikittools.deb", @"launchctl.deb" ])
        assert(!unlink([switch_test_sources stringByAppendingPathComponent:name].fileSystemRepresentation));
    BOOL updated = NO;
    assert(!switch_test_update(NO, &updated) && updated);
    assert(switch_test_commands.count == 3 && switch_test_commands[0].count == 5);
    assert([switch_test_commands[0].lastObject hasSuffix:@"/sileo.deb"]);
    assert(switch_bootstrap_core_current(switch_test_base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256));
    assert([switch_bootstrap_core_state(switch_test_base)[@"custom"] isEqual:@"keep"]);
    [switch_test_commands removeAllObjects];
    assert(!unlink([switch_test_sources stringByAppendingPathComponent:@"sileo.deb"].fileSystemRepresentation));
    assert(!switch_test_update(NO, &updated) && !updated && !switch_test_commands.count);
}

static void switch_test_input_errors(void)
{
    for (unsigned variant = 0; variant != 5; ++variant) {
        switch_test_reset(NO);
        NSString *source = [switch_test_sources stringByAppendingPathComponent:@"sileo.deb"];
        if (!variant)
            switch_test_write(source, @"wrong package");
        else {
            assert(!unlink(source.fileSystemRepresentation));
            if (variant == 1)
                assert(!symlink(switch_test_helper.fileSystemRepresentation, source.fileSystemRepresentation));
            else if (variant == 2)
                assert(!link(switch_test_helper.fileSystemRepresentation, source.fileSystemRepresentation));
            else if (variant == 4)
                assert(!mkfifo(source.fileSystemRepresentation, 0600));
        }
        NSString *before = switch_test_read([switch_test_base stringByAppendingPathComponent:@"manifest.json"]);
        BOOL updated = YES;
        assert(switch_test_update(NO, &updated) && !updated && !switch_test_commands.count);
        assert([switch_test_read([switch_test_base stringByAppendingPathComponent:@"manifest.json"]) isEqual:before]);
    }
}

static void switch_test_retry(void)
{
    for (NSString *action in @[ @"--install", @"--triggers-only", @"hold" ]) {
        switch_test_reset(NO);
        switch_test_failure_action = action;
        NSString *before = switch_test_read([switch_test_base stringByAppendingPathComponent:@"manifest.json"]);
        BOOL updated = YES;
        assert(switch_test_update(NO, &updated) == EIO && !updated);
        assert([switch_test_read([switch_test_base stringByAppendingPathComponent:@"manifest.json"]) isEqual:before]);
        switch_test_failure_action = nil;
        assert(!switch_test_update(NO, &updated) && updated);
        assert(switch_bootstrap_core_current(switch_test_base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256));
    }
}

int main()
{
    @autoreleasepool {
        char temporary[] = "/tmp/SwitchCoreUpdate.XXXXXX";
        assert(mkdtemp(temporary));
        switch_test_base = @(temporary);
        switch_test_root = [switch_test_base stringByAppendingPathComponent:@"root"];
        switch_test_sources = [switch_test_base stringByAppendingPathComponent:@"packages"];
        switch_test_helper = [switch_test_base stringByAppendingPathComponent:@"helper"];
        switch_test_delta();
        switch_test_input_errors();
        switch_test_retry();

        switch_test_reset(YES);
        assert(!switch_bootstrap_verify_installed_core(switch_test_base, switch_test_root));
        switch_test_state(NO);
        assert(switch_bootstrap_verify_installed_core(switch_test_base, switch_test_root) == EAUTH);
        switch_test_state(YES);
        switch_test_installed[@"ellekit"][@"version"] = @"old";
        switch_test_save_status();
        assert(switch_bootstrap_verify_installed_core(switch_test_base, switch_test_root) == EINVAL);
        BOOL updated = NO;
        assert(!switch_test_update(NO, &updated) && updated);
        assert(switch_test_commands.count == 2 && [switch_test_commands[0].lastObject hasSuffix:@"/ellekit.deb"]);

        switch_test_reset(YES);
        switch_test_installed[@"ellekit"][@"status"] = @"install ok half-configured";
        switch_test_save_status();
        assert(!switch_test_update(NO, &updated) && updated);
        assert(switch_test_commands.count == 2 && [switch_test_commands[0].lastObject hasSuffix:@"/ellekit.deb"]);

        switch_test_reset(YES);
        switch_test_installed[@"ellekit"][@"status"] = @"install ok installed";
        switch_test_save_status();
        assert(switch_bootstrap_verify_installed_core(switch_test_base, switch_test_root) == EINVAL);
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_sources error:nil]);
        assert(!switch_test_update(NO, &updated) && !updated && switch_test_commands.count == 1);
        assert([switch_test_commands[0][0] isEqual:@"/var/jb/usr/bin/apt-mark"]);

        switch_test_reset(YES);
        switch_test_installed[@"uikittools"][@"status"] = @"install ok triggers-pending";
        switch_test_installed[@"uikittools"][@"pending"] = @YES;
        switch_test_save_status();
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_sources error:nil]);
        assert(!switch_test_update(NO, &updated) && !updated && switch_test_commands.count == 2);
        assert([switch_test_commands[0] containsObject:@"--triggers-only"]);

        switch_test_reset(NO);
        NSMutableDictionary *seed = [switch_bootstrap_core_state(switch_test_base) mutableCopy];
        seed[@"archive_sha256"] = @SWITCH_BOOTSTRAP_ARCHIVE_SHA256;
        assert(!switch_bootstrap_save_core_state(switch_test_base, seed));
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_sources error:nil]);
        assert(!switch_test_update(NO, &updated) && !updated && !switch_test_commands.count);
        assert(switch_bootstrap_core_current(switch_test_base, @SWITCH_BOOTSTRAP_ARCHIVE_SHA256));

        switch_test_reset(NO);
        seed = [switch_bootstrap_core_state(switch_test_base) mutableCopy];
        seed[@"archive_sha256"] = @SWITCH_BOOTSTRAP_ARCHIVE_SHA256;
        assert(!switch_bootstrap_save_core_state(switch_test_base, seed));
        switch_test_installed[@"ellekit"][@"version"] = @"old";
        switch_test_save_status();
        assert(!switch_test_update(NO, &updated) && updated);
        assert(switch_test_commands[0].count == 4 + switch_bootstrap_core_names().count);

        switch_test_reset(NO);
        NSString *candidate = [switch_test_base stringByAppendingPathComponent:@"core.extracting"];
        assert([NSFileManager.defaultManager createDirectoryAtPath:candidate
                                       withIntermediateDirectories:NO
                                                        attributes:nil
                                                             error:nil]);
        switch_test_write([candidate stringByAppendingPathComponent:@"partial"], @"interrupted");
        assert(!switch_test_update(NO, &updated) && updated);
        assert(![NSFileManager.defaultManager fileExistsAtPath:candidate]);

        switch_test_reset(NO);
        assert(!symlink(switch_test_sources.fileSystemRepresentation, candidate.fileSystemRepresentation));
        assert(switch_test_update(NO, &updated) == EPERM && !updated && !switch_test_commands.count);
        assert([NSFileManager.defaultManager fileExistsAtPath:switch_test_sources]);
        assert(!unlink(candidate.fileSystemRepresentation));

        switch_test_reset(NO);
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_sources error:nil]);
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_helper error:nil]);
        assert(!switch_test_update(YES, &updated) && !updated && !switch_test_commands.count);
        assert([NSFileManager.defaultManager
            fileExistsAtPath:[switch_test_root stringByAppendingPathComponent:@".switch-bootstrap-activated"]]);
        assert([NSFileManager.defaultManager removeItemAtPath:switch_test_base error:nil]);
    }
}
