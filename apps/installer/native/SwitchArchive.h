#pragma once

#import <Foundation/Foundation.h>
#include <sys/types.h>

NS_ASSUME_NONNULL_BEGIN

// iOS exports libarchive's stable C ABI but omits its headers from the SDK.
extern "C" {
struct archive;
struct archive_entry;
struct archive *_Nullable archive_read_new(void);
int archive_read_support_format_zip_seekable(struct archive *);
int archive_read_open_filename(struct archive *, const char *, size_t);
int archive_read_next_header(struct archive *, struct archive_entry *_Nullable *_Nonnull);
ssize_t archive_read_data(struct archive *, void *, size_t);
int archive_read_free(struct archive *);
const char *_Nullable archive_entry_pathname(struct archive_entry *);
const char *_Nullable archive_entry_symlink(struct archive_entry *);
const char *_Nullable archive_entry_hardlink(struct archive_entry *);
int64_t archive_entry_size(struct archive_entry *);
mode_t archive_entry_filetype(struct archive_entry *);
int archive_entry_is_encrypted(struct archive_entry *);
}

NSError *switch_package_error(NSString *reason);
BOOL switch_archive_read(struct archive *archive, void *bytes, size_t length, NSError *_Nullable *_Nullable error);
BOOL switch_validate_executable(struct archive *archive, uint64_t size, NSError *_Nullable *_Nullable error);

NS_ASSUME_NONNULL_END
