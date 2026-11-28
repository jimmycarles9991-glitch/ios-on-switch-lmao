// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "CapabilityPolicy.h"
#import "EntitlementDER.h"

#include <switch/capabilities.h>
#include <algorithm>
#include <cctype>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

NSDictionary *switch_declared_capabilities(NSURL *image)
{
    using SwitchCreateCode = int32_t (*)(CFURLRef, uint32_t, CFTypeRef *);
    using SwitchCopyInfo = int32_t (*)(CFTypeRef, uint32_t, CFDictionaryRef *);
    static void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    static auto create = security ? (SwitchCreateCode)dlsym(security, "SecStaticCodeCreateWithPath") : nullptr;
    static auto copy = security ? (SwitchCopyInfo)dlsym(security, "SecCodeCopySigningInformation") : nullptr;
    static auto key = security ? (CFStringRef *)dlsym(security, "kSecCodeInfoEntitlementsDict") : nullptr;
    if (!image.isFileURL || !create || !copy || !key || !*key)
        return @{};
    CFTypeRef code = nullptr;
    CFDictionaryRef information = nullptr;
    NSDictionary *result = nil;
    if (!create((__bridge CFURLRef)image, 0, &code) && code && !copy(code, 0, &information) && information) {
        id value = ((__bridge NSDictionary *)information)[(__bridge NSString *)*key];
        if ([value isKindOfClass:NSDictionary.class])
            result = [value copy];
    }
    if (information)
        CFRelease(information);
    if (code)
        CFRelease(code);
    return result ?: @{};
}

static BOOL switch_property_value(id value, unsigned depth)
{
    if (depth > SWITCH_CAPABILITIES_MAX_DEPTH)
        return NO;
    if ([value isKindOfClass:NSString.class])
        return [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= SWITCH_CAPABILITIES_MAX_SIZE;
    if ([value isKindOfClass:NSNumber.class])
        return CFGetTypeID((__bridge CFTypeRef)value) == CFBooleanGetTypeID() ||
            (!CFNumberIsFloatType((__bridge CFNumberRef)value) &&
             !([value objCType][0] == 'Q' && [value unsignedLongLongValue] > INT64_MAX));
    if ([value isKindOfClass:NSArray.class]) {
        for (id child in value) {
            if (!switch_property_value(child, depth + 1))
                return NO;
        }
        return YES;
    }
    if ([value isKindOfClass:NSDictionary.class]) {
        for (id key in value) {
            if (![key isKindOfClass:NSString.class] || !switch_property_value(value[key], depth + 1))
                return NO;
        }
        return YES;
    }
    return NO;
}

static BOOL switch_valid_group(id value)
{
    return [value isKindOfClass:NSString.class] && switch_capability_group_allowed([value UTF8String]);
}

static BOOL switch_valid_identity(id value)
{
    return switch_valid_group(value) &&
        [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding] <= SWITCH_CAPABILITIES_IDENTITY_MAX;
}

static NSDictionary *switch_filtered_dictionary(id dictionary)
{
    if (![dictionary isKindOfClass:NSDictionary.class] || !switch_property_value(dictionary, 0))
        return @{};
    NSData *serialized = [NSPropertyListSerialization dataWithPropertyList:dictionary
        format:NSPropertyListXMLFormat_v1_0 options:0 error:nil];
    if (!serialized || serialized.length > SWITCH_CAPABILITIES_MAX_SIZE)
        return @{};
    NSMutableDictionary *result = [NSMutableDictionary dictionary];
    for (id key in dictionary) {
        const char *name = [key UTF8String];
        if (!switch_capability_key_allowed(name))
            continue;
        id value = dictionary[key];
        if (switch_capability_identity_key(name)) {
            if (switch_valid_identity(value))
                result[key] = value;
        } else if (switch_capability_group_key(name)) {
            if (![value isKindOfClass:NSArray.class])
                continue;
            NSMutableArray *groups = [NSMutableArray array];
            for (id group in value) {
                if (switch_valid_group(group))
                    [groups addObject:group];
            }
            if (groups.count)
                result[key] = groups;
        } else {
            result[key] = value;
        }
    }
    return result;
}

NSDictionary *switch_filter_capabilities(id declared, NSString *identity, NSDictionary *inherited)
{
    NSDictionary *filtered = switch_filtered_dictionary(declared);
    NSMutableDictionary *result = [(filtered.count ? filtered : switch_filtered_dictionary(inherited)) mutableCopy];
    if (!result[@"application-identifier"] && result[@"com.apple.application-identifier"])
        result[@"application-identifier"] = result[@"com.apple.application-identifier"];
    if (!result[@"application-identifier"] || !filtered.count) {
        NSString *fallback = identity ?: inherited[@"application-identifier"];
        if (!switch_valid_identity(fallback)) {
            uint64_t hash = UINT64_C(14695981039346656037);
            const unsigned char *bytes = (const unsigned char *)(fallback ?: @"").UTF8String;
            for (; *bytes; ++bytes)
                hash = (hash ^ *bytes) * UINT64_C(1099511628211);
            fallback = [NSString stringWithFormat:@"Switch.%016llx", (unsigned long long)hash];
        }
        result[@"application-identifier"] = fallback;
    }
    if (![result[@"keychain-access-groups"] count])
        result[@"keychain-access-groups"] = @[result[@"application-identifier"]];
    return result;
}

static BOOL switch_posix_failure(NSError **error, int code)
{
    if (error)
        *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
    return NO;
}

static BOOL switch_write_capabilities_at(int directory, const char *imageName, NSDictionary *dictionary,
                                         NSError **error)
{
    NSData *data = switch_serialize_entitlements(dictionary);
    if (!data || data.length > SWITCH_CAPABILITIES_MAX_SIZE)
        return switch_posix_failure(error, EINVAL);
    NSString *name = [[NSString stringWithUTF8String:imageName] stringByAppendingString:@".SwitchEntitlements"];
    int failure = unlinkat(directory, name.fileSystemRepresentation, 0) && errno != ENOENT ? errno : 0;
    int file = failure ? -1 : openat(directory, name.fileSystemRepresentation,
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (file < 0 && !failure)
        failure = errno;
    if (file >= 0) {
        const uint8_t *bytes = (const uint8_t *)data.bytes;
        size_t remaining = data.length;
        while (remaining) {
            ssize_t count = write(file, bytes, remaining);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                failure = count ? errno : EIO;
                break;
            }
            remaining -= (size_t)count;
            bytes += count;
        }
        if (fchmod(file, 0600) && !failure)
            failure = errno;
        if (close(file) && !failure)
            failure = errno;
    }
    return failure ? switch_posix_failure(error, failure) : YES;
}

static BOOL switch_skip_macho_extension(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1])
        return NO;
    const char *ext = dot + 1;
    char lower[24];
    size_t len = 0;
    for (; ext[len]; ++len) {
        if (len >= sizeof(lower) - 1)
            return NO;
        lower[len] = (char)tolower((unsigned char)ext[len]);
    }
    lower[len] = '\0';
    static const char *const ignored[] = {
        "aac", "aif", "aiff", "air", "bmp", "bz2", "caf", "car", "cer", "crt", "css", "csv", "db", "der",
        "entitlements", "flac", "gif", "gz", "h", "heic", "heif", "htm", "html", "icns", "ico", "jpeg", "jpg", "js",
        "json", "m4a", "m4v", "md", "metal", "metallib", "mobileprovision", "mom", "mov", "mp3", "mp4", "nib", "ogg",
        "omo", "otf", "pdf", "pem", "plist", "png", "sh", "sql", "sqlite", "sqlite3", "strings", "stringsdict", "svg",
        "switchentitlements", "tar", "tif", "tiff", "ttc", "ttf", "txt", "wav", "webm", "webp", "woff", "woff2",
        "xcprivacy", "xml", "xz", "yaml", "yml", "zip",
    };
    return std::binary_search(std::begin(ignored), std::end(ignored), lower,
                              [](const char *a, const char *b) { return strcmp(a, b) < 0; });
}

static BOOL switch_is_macho_magic(uint32_t magic)
{
    return magic == 0xfeedfacf || magic == 0xcafebabe || magic == 0xbebafeca || magic == 0xcafebabf ||
           magic == 0xbfbafeca;
}

static BOOL switch_publish_directory(int directory, NSString *relative, NSString *source, NSString *destination,
                                     NSDictionary *images, NSDictionary *bundles, NSUInteger depth, NSError **error)
{
    if (depth > 128)
        return switch_posix_failure(error, E2BIG);
    int enumeration = dup(directory);
    if (enumeration < 0)
        return switch_posix_failure(error, errno);
    DIR *stream = fdopendir(enumeration);
    if (!stream) {
        close(enumeration);
        return switch_posix_failure(error, errno);
    }
    BOOL success = YES;
    errno = 0;
    while (struct dirent *entry = readdir(stream)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        const unsigned char type = entry->d_type;
        if (type != DT_DIR && type != DT_REG && type != DT_UNKNOWN)
            continue;
        if (type == DT_REG && switch_skip_macho_extension(entry->d_name))
            continue;
        if (type == DT_DIR) {
            int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                if (errno != ENOTDIR && errno != ELOOP && errno != ENOENT) {
                    success = switch_posix_failure(error, errno);
                    break;
                }
                errno = 0;
                continue;
            }
            @autoreleasepool {
                NSString *name = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:entry->d_name
                                                                                           length:strlen(entry->d_name)];
                NSString *childRelative = [relative stringByAppendingFormat:@"/%@", name];
                success = switch_publish_directory(child, childRelative, source, destination, images, bundles,
                                                   depth + 1, error);
            }
            close(child);
            if (!success)
                break;
            errno = 0;
            continue;
        }
        int file = openat(directory, entry->d_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (file < 0) {
            errno = 0;
            continue;
        }
        struct stat status;
        if (fstat(file, &status)) {
            close(file);
            errno = 0;
            continue;
        }
        if (type == DT_UNKNOWN && S_ISDIR(status.st_mode)) {
            @autoreleasepool {
                NSString *name = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:entry->d_name
                                                                                           length:strlen(entry->d_name)];
                NSString *childRelative = [relative stringByAppendingFormat:@"/%@", name];
                success = switch_publish_directory(file, childRelative, source, destination, images, bundles,
                                                   depth + 1, error);
            }
            close(file);
            if (!success)
                break;
            errno = 0;
            continue;
        }
        uint32_t magic = 0;
        BOOL executable = S_ISREG(status.st_mode) && !switch_skip_macho_extension(entry->d_name) &&
                          read(file, &magic, sizeof(magic)) == sizeof(magic) && switch_is_macho_magic(magic);
        close(file);
        if (!executable) {
            errno = 0;
            continue;
        }
        NSError *writeError = nil;
        @autoreleasepool {
            NSString *name = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:entry->d_name
                                                                                       length:strlen(entry->d_name)];
            NSString *fileRelative = [relative stringByAppendingFormat:@"/%@", name];
            NSString *original = [source stringByAppendingString:fileRelative];
            NSDictionary *effective = images[original];
            if (!effective) {
                NSDictionary *inherited = nil;
                NSUInteger longest = 0;
                for (NSString *bundle in bundles) {
                    if (bundle.length > longest && [original hasPrefix:[bundle stringByAppendingString:@"/"]]) {
                        inherited = bundles[bundle];
                        longest = bundle.length;
                    }
                }
                NSURL *fileURL = [NSURL fileURLWithPath:[destination stringByAppendingString:fileRelative]];
                effective = switch_filter_capabilities(switch_declared_capabilities(fileURL), nil, inherited);
            }
            success = switch_write_capabilities_at(directory, entry->d_name, effective, &writeError);
        }
        if (!success) {
            if (error)
                *error = writeError;
            break;
        }
        errno = 0;
    }
    if (success && errno)
        success = switch_posix_failure(error, errno);
    closedir(stream);
    return success;
}

BOOL switch_publish_capabilities(NSURL *sourceRoot, NSURL *destinationRoot, NSDictionary *images,
                                 NSDictionary *bundles, NSError **error)
{
    NSString *source = sourceRoot.URLByResolvingSymlinksInPath.path;
    NSString *destination = destinationRoot.URLByResolvingSymlinksInPath.path;
    int directory = destination.length ? open(destination.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory < 0)
        return switch_posix_failure(error, destination.length ? errno : EINVAL);
    BOOL success = switch_publish_directory(directory, @"", source, destination, images, bundles, 0, error);
    close(directory);
    return success;
}
