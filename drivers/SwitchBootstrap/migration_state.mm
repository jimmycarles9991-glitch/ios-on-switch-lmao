// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "migration_state.h"
#include "build_configuration.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/acl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static const char switch_migration_receipt[] = "migration.state";
static const char switch_migration_staging[] = "migration.state.pending";
static constexpr int switch_migration_directory_flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
static constexpr int switch_migration_read_flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;

static bool switch_migration_token(NSString *text, NSString *alphabet, NSUInteger min, NSUInteger max)
{
    return text.length >= min && text.length <= max &&
           [text rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:alphabet].invertedSet]
                   .location == NSNotFound;
}

static bool switch_migration_generation(NSString *text)
{
    return switch_migration_token(text, @"0123456789abcdef", 64, 64);
}

static bool switch_migration_phase(NSString *text)
{
    return switch_migration_token(text, @"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-", 1, 64);
}

static int switch_migration_validate(int fd, dev_t device, bool directory)
{
    struct stat info;
    if (fstat(fd, &info))
        return errno;
    if (device != info.st_dev)
        return EXDEV;
    if (info.st_uid || info.st_gid || (info.st_mode & 0022) ||
        (directory ? !S_ISDIR(info.st_mode) : !S_ISREG(info.st_mode) || info.st_nlink != 1))
        return EPERM;
    acl_t acl = acl_get_fd_np(fd, ACL_TYPE_EXTENDED);
    if (!acl)
        return errno == ENOENT ? 0 : errno;
    acl_free(acl);
    return EPERM;
}

static int switch_migration_directory(NSString *base, int *directory, dev_t *device)
{
    if (![base isEqualToString:@SWITCH_BOOTSTRAP_BASE])
        return EINVAL;
    int fd = open("/private/var", O_RDONLY | O_DIRECTORY | O_NOFOLLOW_ANY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    struct stat info;
    struct statfs volume;
    int error = fstat(fd, &info) || fstatfs(fd, &volume) ? errno : 0;
    if (!error) {
        *device = info.st_dev;
        error = (volume.f_flags & MNT_IGNORE_OWNERSHIP) ? EPERM : switch_migration_validate(fd, *device, true);
    }
    const char *parts[] = {"Switch", "bootstrap", SWITCH_BOOTSTRAP_BUILD};
    for (const char *part : parts) {
        if (error)
            break;
        const int child = openat(fd, part, switch_migration_directory_flags);
        error = child < 0 ? errno : switch_migration_validate(child, *device, true);
        close(fd);
        fd = child;
    }
    if (error && fd >= 0)
        close(fd);
    else if (!error)
        *directory = fd;
    return error;
}

static int switch_migration_parse(NSString *line, NSString *expected, SwitchMigrationStatus *status, NSString **phase,
                                  int *recordedError)
{
    if (![line hasSuffix:@"\n"])
        return EINVAL;
    NSArray<NSString *> *fields = [[line substringToIndex:line.length - 1] componentsSeparatedByString:@" "];
    if (fields.count < 2 || !switch_migration_generation(fields[1]))
        return EINVAL;
    SwitchMigrationStatus value;
    if (fields.count == 2 && [fields[0] isEqualToString:@"complete"]) {
        value = SwitchMigrationComplete;
    } else if (fields.count == 2 && [fields[0] isEqualToString:@"repair"]) {
        value = SwitchMigrationRepair;
    } else if (fields.count == 3 && [fields[0] isEqualToString:@"working"] && switch_migration_phase(fields[2])) {
        value = SwitchMigrationWorking;
    } else if (fields.count == 4 && [fields[0] isEqualToString:@"failed"] && switch_migration_phase(fields[2]) &&
               fields[3].intValue > 0 &&
               [fields[3] isEqualToString:[NSString stringWithFormat:@"%d", fields[3].intValue]]) {
        value = SwitchMigrationFailed;
    } else {
        return EINVAL;
    }
    if ([expected isEqualToString:fields[1]]) {
        *status = value;
        if (phase && fields.count >= 3)
            *phase = fields[2];
        if (recordedError && value == SwitchMigrationFailed)
            *recordedError = fields[3].intValue;
    }
    return 0;
}

int switch_migration_read(NSString *base, NSString *generation, SwitchMigrationStatus *status, NSString **phase,
                          int *recordedError)
{
    if (!status)
        return EINVAL;
    *status = SwitchMigrationMissing;
    if (phase)
        *phase = nil;
    if (recordedError)
        *recordedError = 0;
    if (!switch_migration_generation(generation))
        return EINVAL;
    int directory;
    dev_t device;
    int error = switch_migration_directory(base, &directory, &device);
    if (error)
        return error == ENOENT ? 0 : error;
    const int fd = openat(directory, switch_migration_receipt, switch_migration_read_flags);
    error = fd < 0 ? errno : 0;
    close(directory);
    if (fd < 0)
        return error == ENOENT ? 0 : error;
    error = switch_migration_validate(fd, device, false);
    char bytes[160];
    size_t length = 0;
    while (!error && length < sizeof(bytes)) {
        const ssize_t count = read(fd, bytes + length, sizeof(bytes) - length);
        if (count == 0)
            break;
        if (count < 0) {
            if (errno != EINTR)
                error = errno;
        } else {
            length += (size_t)count;
        }
    }
    close(fd);
    if (error || length == sizeof(bytes))
        return error ?: EINVAL;
    NSString *line = [[NSString alloc] initWithBytes:bytes length:length encoding:NSASCIIStringEncoding];
    return switch_migration_parse(line, generation, status, phase, recordedError);
}

static int switch_migration_check_existing(int directory, dev_t device, const char *name)
{
    const int fd = openat(directory, name, switch_migration_read_flags);
    if (fd < 0)
        return errno == ENOENT ? 0 : errno;
    const int error = switch_migration_validate(fd, device, false);
    close(fd);
    return error;
}

static int switch_migration_save(int directory, dev_t device, NSData *record)
{
    int error = switch_migration_check_existing(directory, device, switch_migration_receipt);
    if (!error)
        error = switch_migration_check_existing(directory, device, switch_migration_staging);
    if (error)
        return error;
    if (unlinkat(directory, switch_migration_staging, 0) && errno != ENOENT)
        return errno;
    const int fd =
        openat(directory, switch_migration_staging, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0)
        return errno;
    error = switch_migration_validate(fd, device, false);
    size_t offset = 0;
    while (!error && offset < record.length) {
        const ssize_t count = write(fd, (const char *)record.bytes + offset, record.length - offset);
        if (count > 0)
            offset += (size_t)count;
        else if (!count || errno != EINTR)
            error = count ? errno : EIO;
    }
    if (!error && fsync(fd))
        error = errno;
    if (close(fd) && !error)
        error = errno;
    if (!error && renameat(directory, switch_migration_staging, directory, switch_migration_receipt))
        error = errno;
    if (error)
        unlinkat(directory, switch_migration_staging, 0);
    else if (fcntl(directory, F_FULLFSYNC))
        error = errno;
    return error;
}

int switch_migration_write(NSString *base, NSString *generation, SwitchMigrationStatus status, const char *phase,
                           int error)
{
    if (!switch_migration_generation(generation))
        return EINVAL;
    NSString *step = phase ? [NSString stringWithUTF8String:phase] : nil;
    NSString *line;
    switch (status) {
    case SwitchMigrationComplete:
    case SwitchMigrationRepair:
        if (error)
            return EINVAL;
        line = [NSString
            stringWithFormat:@"%@ %@\n", status == SwitchMigrationComplete ? @"complete" : @"repair", generation];
        break;
    case SwitchMigrationWorking:
        if (error || !switch_migration_phase(step))
            return EINVAL;
        line = [NSString stringWithFormat:@"working %@ %@\n", generation, step];
        break;
    case SwitchMigrationFailed:
        if (error <= 0 || !switch_migration_phase(step))
            return EINVAL;
        line = [NSString stringWithFormat:@"failed %@ %@ %d\n", generation, step, error];
        break;
    default:
        return EINVAL;
    }
    int directory;
    dev_t device;
    error = switch_migration_directory(base, &directory, &device);
    if (error)
        return error;
    error = switch_migration_save(directory, device, [line dataUsingEncoding:NSASCIIStringEncoding]);
    close(directory);
    return error;
}
