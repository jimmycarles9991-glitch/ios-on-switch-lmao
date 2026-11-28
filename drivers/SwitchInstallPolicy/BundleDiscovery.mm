// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "BundleDiscovery.h"

#include <dirent.h>
#include <fcntl.h>
#include <objc/runtime.h>
#include <sys/stat.h>
#include <unistd.h>

@protocol SwitchDiscoveryBundle <NSObject>
- (NSURL *)bundleURL;
- (id)parentBundle;
+ (NSArray *)bundlesInParentBundle:(id)parent subDirectory:(NSString *)directory
                matchingPredicate:(BOOL (^)(NSURL *))predicate error:(NSError **)error;
@end

static const void *switch_plugin_discovery_key = &switch_plugin_discovery_key;
static const void *switch_extension_discovery_key = &switch_extension_discovery_key;
static const void *switch_discovery_scan_key = &switch_discovery_scan_key;

static BOOL switch_discovery_error(NSError **error, int code)
{
    if (error)
        *error = [NSError errorWithDomain:NSPOSIXErrorDomain code:code userInfo:nil];
    return NO;
}

static void switch_record_extension(NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *groups,
                                    NSString *relative, NSString *name)
{
    NSMutableSet<NSString *> *names = groups[relative];
    if (!names)
        groups[relative] = names = [NSMutableSet set];
    [names addObject:name];
}

static NSDictionary *switch_extension_metadata(int directory, NSError **error)
{
    int file = openat(directory, "Info.plist", O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (file < 0) {
        if (errno != ENOENT && errno != ELOOP)
            switch_discovery_error(error, errno);
        return nil;
    }
    struct stat status;
    NSMutableData *data = nil;
    if (fstat(file, &status)) {
        switch_discovery_error(error, errno);
    } else if (S_ISREG(status.st_mode) && status.st_size > 64 * 1024 * 1024) {
        switch_discovery_error(error, E2BIG);
    } else if (S_ISREG(status.st_mode) && status.st_size > 0) {
        data = [NSMutableData dataWithLength:(NSUInteger)status.st_size];
        size_t offset = 0;
        while (offset < data.length) {
            ssize_t count = read(file, (uint8_t *)data.mutableBytes + offset, data.length - offset);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0) {
                switch_discovery_error(error, count ? errno : EIO);
                data = nil;
                break;
            }
            offset += (size_t)count;
        }
    }
    close(file);
    id dictionary = data ? [NSPropertyListSerialization propertyListWithData:data options:NSPropertyListImmutable
                                                                     format:nil error:nil] : nil;
    return [dictionary isKindOfClass:NSDictionary.class] ? dictionary : nil;
}

static BOOL switch_collect_extensions(int directory, NSString *parentRelative, NSString *selfName,
                                      NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *pluginGroups,
                                      NSMutableDictionary<NSString *, NSMutableSet<NSString *> *> *extensionGroups,
                                      NSUInteger depth, NSUInteger *entries, NSError **error)
{
    if (depth > 128)
        return switch_discovery_error(error, E2BIG);
    int enumeration = dup(directory);
    if (enumeration < 0)
        return switch_discovery_error(error, errno);
    DIR *stream = fdopendir(enumeration);
    if (!stream) {
        close(enumeration);
        return switch_discovery_error(error, errno);
    }
    NSString *currentRelative =
        selfName ? (parentRelative.length ? [parentRelative stringByAppendingPathComponent:selfName] : selfName) : @"";
    BOOL hasInfoPlist = NO;
    BOOL success = YES;
    errno = 0;
    while (struct dirent *entry = readdir(stream)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (++*entries > 1024 * 1024) {
            success = switch_discovery_error(error, E2BIG);
            break;
        }
        if (selfName && !strcmp(entry->d_name, "Info.plist") &&
            (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN)) {
            hasInfoPlist = YES;
            continue;
        }
        if (entry->d_type != DT_DIR && entry->d_type != DT_UNKNOWN)
            continue;
        int child = openat(directory, entry->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) {
            if (errno != ENOTDIR && errno != ELOOP && errno != ENOENT) {
                success = switch_discovery_error(error, errno);
                break;
            }
            errno = 0;
            continue;
        }
        NSString *name = [[NSFileManager defaultManager] stringWithFileSystemRepresentation:entry->d_name
                                                                                   length:strlen(entry->d_name)];
        success = switch_collect_extensions(child, currentRelative, name, pluginGroups, extensionGroups, depth + 1,
                                            entries, error);
        close(child);
        if (!success)
            break;
        errno = 0;
    }
    if (success && errno)
        success = switch_discovery_error(error, errno);
    closedir(stream);
    if (success && hasInfoPlist) {
        NSError *metadataError = nil;
        BOOL modern = NO, legacy = NO;
        @autoreleasepool {
            NSDictionary *metadata = switch_extension_metadata(directory, &metadataError);
            modern = [metadata[@"EXAppExtensionAttributes"] isKindOfClass:NSDictionary.class];
            legacy = [metadata[@"NSExtension"] isKindOfClass:NSDictionary.class];
        }
        if (metadataError) {
            if (error)
                *error = metadataError;
            return NO;
        }
        if (modern)
            switch_record_extension(extensionGroups, parentRelative, selfName);
        else if (legacy)
            switch_record_extension(pluginGroups, parentRelative, selfName);
    }
    return success;
}

static NSArray<NSDictionary *> *switch_scan_extensions(id parent, NSURL *root, NSError **error)
{
    NSArray<NSDictionary *> *scan = objc_getAssociatedObject(parent, switch_discovery_scan_key);
    if (scan)
        return scan;
    int directory = root.isFileURL ? open(root.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (directory < 0) {
        switch_discovery_error(error, root.isFileURL ? errno : EINVAL);
        return nil;
    }
    NSMutableDictionary *pluginGroups = [NSMutableDictionary dictionary];
    NSMutableDictionary *extensionGroups = [NSMutableDictionary dictionary];
    NSUInteger entries = 0;
    BOOL success = switch_collect_extensions(directory, @"", nil, pluginGroups, extensionGroups, 0, &entries, error);
    close(directory);
    if (!success)
        return nil;
    scan = @[ pluginGroups, extensionGroups ];
    objc_setAssociatedObject(parent, switch_discovery_scan_key, scan, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return scan;
}

NSArray *switch_discover_extensions(id parent, Class nativeClass, BOOL extensionKit,
                                    NSArray *known, NSError **error)
{
    if ([parent parentBundle])
        return @[];
    const void *key = extensionKit ? switch_extension_discovery_key : switch_plugin_discovery_key;
    NSArray *cached = objc_getAssociatedObject(parent, key);
    if (cached)
        return cached;
    SEL selector = @selector(bundlesInParentBundle:subDirectory:matchingPredicate:error:);
    if (!nativeClass || ![nativeClass respondsToSelector:selector]) {
        switch_discovery_error(error, ENOTSUP);
        return nil;
    }
    NSURL *root = [parent bundleURL].URLByResolvingSymlinksInPath;
    NSArray<NSDictionary *> *scan = switch_scan_extensions(parent, root, error);
    if (!scan)
        return nil;
    NSDictionary *groups = scan[extensionKit ? 1 : 0];
    NSMutableDictionary *byPath = [NSMutableDictionary dictionary];
    for (id<SwitchDiscoveryBundle> bundle in known) {
        NSString *path = bundle.bundleURL.URLByResolvingSymlinksInPath.path;
        NSString *relative = [path hasPrefix:[root.path stringByAppendingString:@"/"]] ?
            [path substringFromIndex:root.path.length + 1] : nil;
        if (relative && [groups[relative.stringByDeletingLastPathComponent] containsObject:relative.lastPathComponent])
            byPath[path] = bundle;
    }
    for (NSString *relative in [[groups allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
        NSSet *names = groups[relative];
        NSArray *bundles = [(Class<SwitchDiscoveryBundle>)nativeClass bundlesInParentBundle:parent subDirectory:relative
            matchingPredicate:^BOOL(NSURL *url) { return [names containsObject:url.lastPathComponent]; } error:error];
        if (!bundles)
            return nil;
        for (id<SwitchDiscoveryBundle> bundle in bundles) {
            NSString *path = bundle.bundleURL.URLByResolvingSymlinksInPath.path;
            if (!byPath[path])
                byPath[path] = bundle;
        }
    }
    NSMutableArray *result = [NSMutableArray arrayWithCapacity:byPath.count];
    for (NSString *path in [[byPath allKeys] sortedArrayUsingSelector:@selector(compare:)])
        [result addObject:byPath[path]];
    cached = [result copy];
    objc_setAssociatedObject(parent, key, cached, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    if (error)
        *error = nil;
    return cached;
}
