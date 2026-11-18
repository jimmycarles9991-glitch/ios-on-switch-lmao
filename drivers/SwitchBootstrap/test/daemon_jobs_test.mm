// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>

#include <cassert>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

#include "drivers/SwitchBootstrap/daemon_jobs.mm"

static NSMutableArray<NSString *> *switch_test_actions;
static std::map<std::string, int> switch_test_results;

BOOL switch_bootstrap_metadata(NSString *path, BOOL directory)
{
    struct stat info;
    return !lstat(path.fileSystemRepresentation, &info) && info.st_uid == getuid() && !(info.st_mode & 0022) &&
           (directory ? S_ISDIR(info.st_mode) : S_ISREG(info.st_mode) && info.st_nlink == 1);
}

extern "C" int switch_bootstrap_run_daemon(const char *action, const char *plist, int *status)
{
    NSString *name = [@(plist) lastPathComponent];
    [switch_test_actions addObject:[NSString stringWithFormat:@"%s %@", action, name]];
    if (!strcmp(action, "bootout")) {
        *status = SWITCH_LAUNCHCTL_ABSENT;
        return 0;
    }
    const int result = switch_test_results[name.UTF8String];
    if (result == SWITCH_LAUNCHCTL_FAILED)
        return EIO;
    *status = result;
    return 0;
}

static NSString *switch_test_job(NSString *directory, NSString *filename)
{
    NSString *path = [directory stringByAppendingPathComponent:filename];
    NSDictionary *job = @{@"Label" : filename, @"ProgramArguments" : @[ @"/var/jb/bin/sh" ]};
    assert([job writeToFile:path atomically:YES]);
    return path;
}

int main()
{
    @autoreleasepool {
        char temporary[] = "/private/tmp/SwitchDaemons.XXXXXX";
        assert(mkdtemp(temporary));
        NSString *root = @(temporary);
        NSString *directory = [root stringByAppendingPathComponent:@"Library/LaunchDaemons"];
        NSFileManager *files = NSFileManager.defaultManager;
        switch_test_actions = [NSMutableArray new];
        NSMutableArray *loaded = [NSMutableArray new];
        assert(!switch_bootstrap_load_daemons(root, loaded) && !loaded.count);
        assert([files createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]);

        switch_test_job(directory, @"00-disabled.plist");
        switch_test_job(directory, @"01-existing.plist");
        NSString *fresh = switch_test_job(directory, @"02-new.plist");
        switch_test_results["00-disabled.plist"] = SWITCH_LAUNCHCTL_DISABLED;
        switch_test_results["01-existing.plist"] = SWITCH_LAUNCHCTL_PRESENT;
        assert(!switch_bootstrap_load_daemons(root, loaded));
        assert(([loaded isEqual:@[ fresh ]]));
        assert(([switch_test_actions
            isEqual:@[ @"bootstrap 00-disabled.plist", @"bootstrap 01-existing.plist", @"bootstrap 02-new.plist" ]]));
        switch_bootstrap_unload_daemons(loaded);
        assert([switch_test_actions.lastObject isEqual:@"bootout 02-new.plist"]);

        [loaded removeAllObjects];
        [switch_test_actions removeAllObjects];
        switch_test_results["02-new.plist"] = SWITCH_LAUNCHCTL_PRESENT;
        assert(!switch_bootstrap_load_daemons(root, loaded) && !loaded.count);
        switch_bootstrap_unload_daemons(loaded);
        assert(switch_test_actions.count == 3);

        [switch_test_actions removeAllObjects];
        switch_test_results["02-new.plist"] = SWITCH_LAUNCHCTL_CHANGED;
        NSString *second = switch_test_job(directory, @"03-second.plist");
        switch_test_job(directory, @"04-invalid.plist");
        switch_test_results["04-invalid.plist"] = SWITCH_LAUNCHCTL_FAILED;
        assert(switch_bootstrap_load_daemons(root, loaded) == EIO);
        assert(([loaded isEqual:@[ fresh, second ]]));
        switch_bootstrap_unload_daemons(loaded);
        assert(([switch_test_actions isEqual:@[
            @"bootstrap 00-disabled.plist", @"bootstrap 01-existing.plist", @"bootstrap 02-new.plist",
            @"bootstrap 03-second.plist", @"bootstrap 04-invalid.plist", @"bootout 03-second.plist",
            @"bootout 02-new.plist"
        ]]));

        assert([files removeItemAtPath:directory error:nil]);
        assert([files createDirectoryAtPath:directory withIntermediateDirectories:YES attributes:nil error:nil]);
        NSString *bad = [directory stringByAppendingPathComponent:@"invalid.plist"];
        assert([@"invalid" writeToFile:bad atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        [loaded removeAllObjects];
        [switch_test_actions removeAllObjects];
        assert(switch_bootstrap_load_daemons(root, loaded) == EINVAL);
        assert(!loaded.count && !switch_test_actions.count);
        assert([files removeItemAtPath:bad error:nil]);
        assert(!symlink("/etc/hosts", bad.fileSystemRepresentation));
        assert(switch_bootstrap_load_daemons(root, loaded) == EPERM);
        assert([files removeItemAtPath:root error:nil]);
    }
}
