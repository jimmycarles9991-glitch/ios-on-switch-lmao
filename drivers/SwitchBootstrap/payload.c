// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "archive.h"
#include "payload.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool switch_bootstrap_relative_path(const char *path)
{
    if (!path || !*path || *path == '/' || strlen(path) >= PATH_MAX)
        return false;
    for (const char *part = path; *part;) {
        const char *end = strchr(part, '/');
        const size_t size = end ? (size_t)(end - part) : strlen(part);
        if (!size || (size == 2 && !memcmp(part, "..", 2)))
            return false;
        part += size;
        if (*part)
            ++part;
    }
    return true;
}

static const char *const switch_bootstrap_core_files[] = {
    "usr/share/Switch/bootstrap-packages/sileo.deb",
    "usr/share/Switch/bootstrap-packages/ellekit.deb",
    "usr/share/Switch/bootstrap-packages/uikittools.deb",
    "usr/share/Switch/bootstrap-packages/launchctl.deb",
};

static int switch_bootstrap_extract_archive(const char *archive_path, const char *directory, bool core_only)
{
    const int previous = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (previous < 0)
        return errno;
    int error = 0;
    struct archive *input = archive_read_new();
    struct archive *output = archive_write_disk_new();
    if (!input || !output)
        error = ENOMEM;
    // libarchive enforces the same boundary while resolving hard links and deferred directory metadata.
    const int flags = 0x0001 | 0x0002 | 0x0004 | 0x0100 | 0x0200 | 0x10000;
    if (!error && (archive_read_support_filter_all(input) < 0 || archive_read_support_format_tar(input) < 0 ||
                   archive_write_disk_set_options(output, flags) < 0 ||
                   archive_read_open_filename(input, archive_path, 65536) < 0))
        error = EINVAL;
    if (!error && chdir(directory))
        error = errno;
    bool seen[sizeof(switch_bootstrap_core_files) / sizeof(*switch_bootstrap_core_files)] = {false};
    struct archive_entry *entry = NULL;
    while (!error) {
        const int result = archive_read_next_header(input, &entry);
        if (result == 1)
            break;
        if (result != 0) {
            error = EINVAL;
            break;
        }
        const unsigned type = archive_entry_filetype(entry);
        const char *path = archive_entry_pathname(entry);
        const char *link = archive_entry_hardlink(entry);
        if (core_only) {
            size_t index = 0;
            for (; index < sizeof(seen) / sizeof(*seen); ++index)
                if (path && !strcmp(path, switch_bootstrap_core_files[index]))
                    break;
            if (index == sizeof(seen) / sizeof(*seen)) {
                if (archive_read_data_skip(input) != 0)
                    error = EINVAL;
                continue;
            }
            if (seen[index] || type != S_IFREG || link || archive_entry_symlink(entry) ||
                archive_entry_uid(entry) != getuid() || archive_entry_gid(entry) != getgid() ||
                archive_entry_perm(entry) != 0644) {
                error = EINVAL;
                break;
            }
            seen[index] = true;
        }
        if (!switch_bootstrap_relative_path(path) || (link && !switch_bootstrap_relative_path(link)) ||
            (type != S_IFDIR && type != S_IFREG && type != S_IFLNK && !link) || archive_entry_uid(entry) < 0 ||
            archive_entry_uid(entry) > UINT32_MAX || archive_entry_gid(entry) < 0 ||
            archive_entry_gid(entry) > UINT32_MAX) {
            error = EINVAL;
            break;
        }
        if (archive_read_extract2(input, entry, output) != 0)
            error = EIO;
    }
    if (error && input) {
        const char *message = archive_error_string(input);
        fprintf(stderr, "SwitchBootstrap: archive: %s\n", message ? message : strerror(error));
    }
    if (output && archive_write_free(output) != 0 && !error)
        error = EIO;
    if (input && archive_read_free(input) != 0 && !error)
        error = EIO;
    if (!error && core_only) {
        for (size_t index = 0; index < sizeof(seen) / sizeof(*seen); ++index) {
            struct stat info;
            if (!seen[index] || lstat(switch_bootstrap_core_files[index], &info) || !S_ISREG(info.st_mode) ||
                info.st_nlink != 1 || info.st_uid != getuid() || info.st_gid != getgid() ||
                (info.st_mode & 07777) != 0644) {
                error = EINVAL;
                break;
            }
        }
    }
    if (fchdir(previous) && !error)
        error = errno;
    close(previous);
    return error;
}

int switch_bootstrap_extract(const char *archive_path, const char *directory)
{
    return switch_bootstrap_extract_archive(archive_path, directory, false);
}

int switch_bootstrap_extract_core(const char *archive_path, const char *directory)
{
    return switch_bootstrap_extract_archive(archive_path, directory, true);
}
