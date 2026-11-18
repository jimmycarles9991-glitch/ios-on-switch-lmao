// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

struct archive;
struct archive_entry;

struct archive *archive_read_new(void);
int archive_read_support_filter_all(struct archive *);
int archive_read_support_format_tar(struct archive *);
int archive_read_open_filename(struct archive *, const char *, size_t);
int archive_read_next_header(struct archive *, struct archive_entry **);
int archive_read_data_skip(struct archive *);
int archive_read_extract2(struct archive *, struct archive_entry *, struct archive *);
int archive_read_free(struct archive *);
const char *archive_error_string(struct archive *);
struct archive *archive_write_disk_new(void);
int archive_write_disk_set_options(struct archive *, int);
int archive_write_free(struct archive *);
const char *archive_entry_pathname(struct archive_entry *);
const char *archive_entry_hardlink(struct archive_entry *);
const char *archive_entry_symlink(struct archive_entry *);
unsigned short archive_entry_filetype(struct archive_entry *);
mode_t archive_entry_perm(struct archive_entry *);
int64_t archive_entry_uid(struct archive_entry *);
int64_t archive_entry_gid(struct archive_entry *);
