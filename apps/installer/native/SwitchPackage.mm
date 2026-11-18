#import "SwitchPackage.h"
#import "SwitchArchive.h"

#include <sys/stat.h>

NSError *switch_package_error(NSString *reason)
{
    return [NSError errorWithDomain:@"com.brunostjohn.switch.installer.package"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : reason}];
}

static id switch_package_fail(NSError **error, NSString *reason)
{
    if (error)
        *error = switch_package_error(reason);
    return nil;
}

@interface SwitchPackageArchive : NSObject
@property (nonatomic, readonly) struct archive *reader;
- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error;
- (nullable NSData *)readEntry:(struct archive_entry *)entry limit:(NSUInteger)limit error:(NSError **)error;
@end

@implementation SwitchPackageArchive

- (instancetype)initWithURL:(NSURL *)url error:(NSError **)error
{
    if (!(self = [super init]))
        return nil;
    _reader = archive_read_new();
    if (!_reader || archive_read_support_format_zip_seekable(_reader) != 0 ||
        archive_read_open_filename(_reader, url.fileSystemRepresentation, 65536) != 0)
        return switch_package_fail(error, @"This file is not a readable IPA archive. Download the IPA again.");
    return self;
}

- (void)dealloc
{
    if (_reader)
        archive_read_free(_reader);
}

- (NSData *)readEntry:(struct archive_entry *)entry limit:(NSUInteger)limit error:(NSError **)error
{
    int64_t size = archive_entry_size(entry);
    if (size <= 0 || (uint64_t)size > limit)
        return switch_package_fail(error, @"The IPA contains invalid or oversized app metadata.");
    NSMutableData *data = [NSMutableData dataWithLength:(NSUInteger)size];
    return switch_archive_read(_reader, data.mutableBytes, data.length, error) ? data : nil;
}

@end

static BOOL switch_safe_path(NSString *path)
{
    if (!path.length || path.length > 4096 || [path hasPrefix:@"/"] || [path containsString:@"\\"])
        return NO;
    NSArray *parts = [path componentsSeparatedByString:@"/"];
    for (NSUInteger index = 0; index < parts.count; ++index) {
        NSString *part = parts[index];
        if ([part isEqualToString:@"."] || [part isEqualToString:@".."] || (!part.length && index + 1 < parts.count))
            return NO;
    }
    return YES;
}

static BOOL switch_safe_link(NSString *path, const char *target)
{
    if (!target)
        return YES;
    NSString *link = [NSString stringWithUTF8String:target];
    if (!link.length || link.length > 4096 || [link hasPrefix:@"/"] || [link containsString:@"\\"])
        return NO;
    NSArray *parts = [path componentsSeparatedByString:@"/"];
    if (parts.count < 3 || ![parts[0] isEqualToString:@"Payload"] || ![parts[1] hasSuffix:@".app"])
        return NO;
    NSMutableArray *resolved = [[path.stringByDeletingLastPathComponent componentsSeparatedByString:@"/"] mutableCopy];
    for (NSString *part in [link componentsSeparatedByString:@"/"]) {
        if ([part isEqualToString:@".."]) {
            if (resolved.count <= 2)
                return NO;
            [resolved removeLastObject];
        } else if (part.length && ![part isEqualToString:@"."]) {
            [resolved addObject:part];
        }
    }
    return resolved.count > 2;
}

static NSString *switch_string(id value)
{
    return [value isKindOfClass:NSString.class] && [value length] && [value length] <= 1024 ? value : nil;
}

static BOOL switch_inspect_entry(SwitchPackageArchive *archive, struct archive_entry *entry,
                                 NSMutableSet<NSString *> *paths, NSDictionary **metadata, NSString **root,
                                 uint64_t *expanded, NSError **error)
{
    const char *rawPath = archive_entry_pathname(entry);
    NSString *path = rawPath ? [NSString stringWithUTF8String:rawPath] : nil;
    if (!switch_safe_path(path) || [paths containsObject:path] || paths.count >= 100000 ||
        !switch_safe_link(path, archive_entry_symlink(entry)) || archive_entry_hardlink(entry)) {
        switch_package_fail(error, @"The IPA contains unsafe or duplicate archive paths.");
        return NO;
    }
    [paths addObject:path];
    mode_t type = archive_entry_filetype(entry);
    int64_t size = archive_entry_size(entry);
    if ((type != S_IFREG && type != S_IFDIR && type != S_IFLNK) || size < 0 ||
        (uint64_t)size > (UINT64_C(16) << 30) - *expanded || archive_entry_is_encrypted(entry)) {
        switch_package_fail(error, @"The IPA contains unsupported entries or expands beyond 16 GB.");
        return NO;
    }
    *expanded += size;
    NSArray<NSString *> *parts = [path componentsSeparatedByString:@"/"];
    if (parts.count >= 2 && [parts[0] isEqualToString:@"Payload"] && [parts[1] hasSuffix:@".app"]) {
        NSString *candidate = [NSString stringWithFormat:@"Payload/%@/", parts[1]];
        if (*root && ![*root isEqualToString:candidate]) {
            switch_package_fail(error, @"An IPA must contain exactly one app in its Payload folder.");
            return NO;
        }
        *root = candidate;
    }
    if (parts.count != 3 || ![parts[0] isEqualToString:@"Payload"] || ![parts[1] hasSuffix:@".app"] ||
        ![parts[2] isEqualToString:@"Info.plist"])
        return YES;
    if (*metadata || type != S_IFREG) {
        switch_package_fail(error, @"An IPA must contain exactly one app in its Payload folder.");
        return NO;
    }
    NSData *data = [archive readEntry:entry limit:1024 * 1024 error:error];
    if (!data)
        return NO;
    id plist = [NSPropertyListSerialization propertyListWithData:data
                                                         options:NSPropertyListImmutable
                                                          format:nil
                                                           error:error];
    if (![plist isKindOfClass:NSDictionary.class]) {
        switch_package_fail(error, @"The IPA's app information is not a valid property list.");
        return NO;
    }
    *metadata = plist;
    *root = [NSString stringWithFormat:@"Payload/%@/", parts[1]];
    return YES;
}

@implementation SwitchPackage

+ (instancetype)inspectURL:(NSURL *)url systemVersion:(NSString *)systemVersion error:(NSError **)error
{
    if (!url.isFileURL)
        return switch_package_fail(error, @"Choose a downloaded IPA file.");
    SwitchPackageArchive *archive = [[SwitchPackageArchive alloc] initWithURL:url error:error];
    if (!archive)
        return nil;
    NSMutableSet<NSString *> *paths = [NSMutableSet set];
    NSDictionary *metadata = nil;
    NSString *root = nil;
    uint64_t expanded = 0;
    struct archive_entry *entry;
    int status;
    while ((status = archive_read_next_header(archive.reader, &entry)) == 0) {
        NSError *failure = nil;
        BOOL valid;
        @autoreleasepool {
            valid = switch_inspect_entry(archive, entry, paths, &metadata, &root, &expanded, &failure);
        }
        if (!valid) {
            if (error)
                *error = failure;
            return nil;
        }
    }
    if (status != 1)
        return switch_package_fail(error, @"The IPA archive is damaged. Download it again.");
    NSString *identifier = switch_string(metadata[@"CFBundleIdentifier"]);
    NSString *executable = switch_string(metadata[@"CFBundleExecutable"]);
    NSCharacterSet *invalid = [[NSCharacterSet
        characterSetWithCharactersInString:@"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-"]
        invertedSet];
    if (!identifier || identifier.length > 255 || [identifier rangeOfCharacterFromSet:invalid].location != NSNotFound ||
        !executable || [executable containsString:@"/"] || !switch_safe_path(executable) ||
        ![metadata[@"CFBundlePackageType"] isEqual:@"APPL"])
        return switch_package_fail(error, @"The IPA is missing a valid app identifier or executable.");
    if ([identifier.lowercaseString hasPrefix:@"com.apple."] ||
        [identifier.lowercaseString isEqualToString:@"com.brunostjohn.switch.installer"])
        return switch_package_fail(error, @"Installer cannot replace this system app.");
    NSString *minimum = switch_string(metadata[@"MinimumOSVersion"]) ?: @"0";
    NSCharacterSet *versionCharacters =
        [[NSCharacterSet characterSetWithCharactersInString:@"0123456789."] invertedSet];
    if ([minimum rangeOfCharacterFromSet:versionCharacters].location != NSNotFound || [minimum hasPrefix:@"."] ||
        [minimum hasSuffix:@"."] || [minimum containsString:@".."])
        return switch_package_fail(error, @"The IPA has an invalid minimum iOS version.");
    if ([minimum compare:systemVersion options:NSNumericSearch] == NSOrderedDescending)
        return switch_package_fail(
            error,
            [NSString stringWithFormat:@"This app requires iOS %@. This device runs iOS %@.", minimum, systemVersion]);
    archive = [[SwitchPackageArchive alloc] initWithURL:url error:error];
    if (!archive)
        return nil;
    NSString *binary = [root stringByAppendingString:executable];
    NSData *icon = nil;
    BOOL found = NO;
    while ((status = archive_read_next_header(archive.reader, &entry)) == 0) {
        const char *rawPath = archive_entry_pathname(entry);
        NSString *path = rawPath ? [NSString stringWithUTF8String:rawPath] : nil;
        if ([path isEqualToString:binary]) {
            if (archive_entry_filetype(entry) != S_IFREG ||
                !switch_validate_executable(archive.reader, archive_entry_size(entry), error))
                return nil;
            found = YES;
        } else if (!icon && [path hasPrefix:root] &&
                   [path.stringByDeletingLastPathComponent isEqualToString:[root substringToIndex:root.length - 1]] &&
                   [path.lastPathComponent.lowercaseString hasPrefix:@"appicon"] &&
                   [path.pathExtension.lowercaseString isEqualToString:@"png"] &&
                   archive_entry_filetype(entry) == S_IFREG && archive_entry_size(entry) > 0 &&
                   archive_entry_size(entry) <= 4 * 1024 * 1024) {
            icon = [archive readEntry:entry limit:4 * 1024 * 1024 error:nil];
        }
    }
    if (status != 1 || !found)
        return switch_package_fail(error, @"The IPA does not contain its app executable.");
    SwitchPackage *package = [self new];
    package->_name = [switch_string(metadata[@"CFBundleDisplayName"]) ?: switch_string(metadata[@"CFBundleName"]) ?:
        identifier copy];
    package->_identifier = [identifier copy];
    package->_version = [switch_string(metadata[@"CFBundleShortVersionString"]) ?:
        switch_string(metadata[@"CFBundleVersion"]) ?: @"Unknown" copy];
    package->_minimumOSVersion = [minimum copy];
    package->_expandedSize = expanded;
    package->_iconData = icon;
    return package;
}

@end
