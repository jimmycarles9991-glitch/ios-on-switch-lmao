// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "CapabilityPolicy.h"
#import "EntitlementDER.h"

#include <switch/capabilities.h>
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

static BOOL switch_write_capabilities(NSURL *image, NSDictionary *dictionary, NSError **error)
{
    NSData *data = switch_serialize_entitlements(dictionary);
    if (!data || data.length > SWITCH_CAPABILITIES_MAX_SIZE)
        return switch_posix_failure(error, EINVAL);
    int directory = open(image.URLByDeletingLastPathComponent.fileSystemRepresentation,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0)
        return switch_posix_failure(error, errno);
    NSString *name = [image.lastPathComponent stringByAppendingString:@".SwitchEntitlements"];
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
    close(directory);
    return failure ? switch_posix_failure(error, failure) : YES;
}

BOOL switch_publish_capabilities(NSURL *sourceRoot, NSURL *destinationRoot, NSDictionary *images,
                                 NSDictionary *bundles, NSError **error)
{
    NSString *source = sourceRoot.URLByResolvingSymlinksInPath.path;
    NSString *destination = destinationRoot.URLByResolvingSymlinksInPath.path;
    __block NSError *enumerationError = nil;
    NSDirectoryEnumerator *enumerator = [[NSFileManager defaultManager] enumeratorAtURL:destinationRoot.URLByResolvingSymlinksInPath
        includingPropertiesForKeys:nil options:0 errorHandler:^BOOL(NSURL *url, NSError *failure) {
            (void)url;
            enumerationError = failure;
            return NO;
        }];
    for (NSURL *url in enumerator) {
        int file = open(url.fileSystemRepresentation, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (file < 0)
            continue;
        struct stat status;
        uint32_t magic = 0;
        BOOL executable = !fstat(file, &status) && S_ISREG(status.st_mode) && read(file, &magic, sizeof(magic)) == sizeof(magic) &&
            (magic == 0xfeedfacf || magic == 0xcafebabe || magic == 0xbebafeca || magic == 0xcafebabf || magic == 0xbfbafeca);
        close(file);
        if (!executable)
            continue;
        NSString *path = url.URLByResolvingSymlinksInPath.path;
        if (![path hasPrefix:[destination stringByAppendingString:@"/"]])
            continue;
        NSString *relative = [path substringFromIndex:destination.length];
        NSString *original = [source stringByAppendingString:relative];
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
            effective = switch_filter_capabilities(switch_declared_capabilities(url), nil, inherited);
        }
        if (!switch_write_capabilities(url, effective, error))
            return NO;
    }
    if (enumerationError) {
        if (error)
            *error = enumerationError;
        return NO;
    }
    return YES;
}
