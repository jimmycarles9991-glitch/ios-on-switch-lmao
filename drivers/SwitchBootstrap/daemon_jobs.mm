// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "daemon_jobs.h"
#import "state.h"
#include "process.h"
#include "bootstrap/launchctl/daemon_result.h"

#include <errno.h>
#include <os/log.h>
#include <sys/stat.h>

int switch_bootstrap_load_daemons(NSString *root, NSMutableArray<NSString *> *loaded)
{
    NSString *directory = [root stringByAppendingPathComponent:@"Library/LaunchDaemons"];
    struct stat metadata;
    if (lstat(directory.fileSystemRepresentation, &metadata))
        return errno == ENOENT ? 0 : errno;
    if (!switch_bootstrap_metadata(directory, YES))
        return EPERM;
    NSError *error = nil;
    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:directory error:&error];
    if (!names)
        return EIO;
    for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
        if (![name.pathExtension isEqual:@"plist"])
            continue;
        NSString *path = [directory stringByAppendingPathComponent:name];
        if (!switch_bootstrap_metadata(path, NO))
            return EPERM;
        NSDictionary *job = [NSDictionary dictionaryWithContentsOfFile:path];
        if (![job isKindOfClass:NSDictionary.class] || ![job[@"Label"] isKindOfClass:NSString.class] ||
            ![job[@"Label"] length])
            return EINVAL;
        int status = SWITCH_LAUNCHCTL_FAILED;
        const int result = switch_bootstrap_run_daemon("bootstrap", path.fileSystemRepresentation, &status);
        if (result) {
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: daemon %{public}@ failed: %d", path, result);
            return result;
        }
        if (status == SWITCH_LAUNCHCTL_CHANGED)
            [loaded addObject:path];
        else if (status != SWITCH_LAUNCHCTL_PRESENT && status != SWITCH_LAUNCHCTL_DISABLED)
            return EIO;
    }
    return 0;
}

void switch_bootstrap_unload_daemons(NSArray<NSString *> *loaded)
{
    for (NSString *path in loaded.reverseObjectEnumerator) {
        int status = SWITCH_LAUNCHCTL_FAILED;
        const int error = switch_bootstrap_run_daemon("bootout", path.fileSystemRepresentation, &status);
        if (error || (status != SWITCH_LAUNCHCTL_CHANGED && status != SWITCH_LAUNCHCTL_ABSENT))
            os_log_error(OS_LOG_DEFAULT, "SwitchBootstrap: daemon rollback %{public}@ failed: %d", path, error ?: EIO);
    }
}
