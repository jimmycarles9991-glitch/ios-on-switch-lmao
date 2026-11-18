// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "bootstrap_registration.h"
#import "app_registration.h"
#import "installer_handler.h"
#include "build_configuration.h"
#include "uicache_options.h"

#include <errno.h>
#include <fcntl.h>
#include <os/log.h>
#include <sys/stat.h>
#include <sysexits.h>
#include <unistd.h>

static NSString *switch_apps_root = @SWITCH_BOOTSTRAP_ROOT;

static NSString *switch_app_path(NSString *path)
{
    if (![path isKindOfClass:NSString.class])
        return nil;
    const std::string canonical = switch_uicache_app_path(switch_apps_root.UTF8String, path.UTF8String);
    return canonical.empty() ? nil : @(canonical.c_str());
}

static int switch_checked_file(int fd, BOOL directory, struct stat *metadata)
{
    filesec_t security = filesec_init();
    if (!security)
        return ENOMEM;
    int hasACL = 0;
    int error = 0;
    if (fstatx_np(fd, metadata, security) || filesec_query_property(security, FILESEC_ACL, &hasACL))
        error = errno;
    filesec_free(security);
    if (error)
        return error;
    if (metadata->st_uid != 0 || metadata->st_gid != 0 || (metadata->st_mode & 0022) || hasACL)
        return EPERM;
    if (directory ? !S_ISDIR(metadata->st_mode) : !S_ISREG(metadata->st_mode) || metadata->st_nlink != 1)
        return EFTYPE;
    return 0;
}

static int switch_open_directory(NSString *path, int *result)
{
    const int flags = O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC;
    int parent = open("/", flags);
    if (parent < 0)
        return errno;
    struct stat metadata;
    int error = switch_checked_file(parent, YES, &metadata);
    NSArray<NSString *> *components = path.pathComponents;
    for (NSUInteger index = 1; !error && index < components.count; ++index) {
        const int child = openat(parent, components[index].fileSystemRepresentation, flags);
        if (child < 0) {
            error = errno;
            break;
        }
        close(parent);
        parent = child;
        error = switch_checked_file(parent, YES, &metadata);
    }
    if (error)
        close(parent);
    else
        *result = parent;
    return error;
}

static NSDictionary *switch_bundle_info(NSString *path, int *error)
{
    int bundle = -1;
    *error = switch_open_directory(path, &bundle);
    if (*error)
        return nil;
    const int flags = O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK;
    const int info = openat(bundle, "Info.plist", flags);
    struct stat metadata;
    *error = info < 0 ? errno : switch_checked_file(info, NO, &metadata);
    NSData *data = nil;
    if (info >= 0) {
        NSFileHandle *handle = [[NSFileHandle alloc] initWithFileDescriptor:info closeOnDealloc:YES];
        if (!*error && (metadata.st_size <= 0 || metadata.st_size > 1024 * 1024))
            *error = EFBIG;
        if (!*error) {
            data = [handle readDataUpToLength:(NSUInteger)metadata.st_size error:nil];
            if (data.length != (NSUInteger)metadata.st_size)
                *error = EIO;
        }
    }
    id plist = data ? [NSPropertyListSerialization propertyListWithData:data
                                                                options:NSPropertyListImmutable
                                                                 format:NULL
                                                                  error:nil]
                    : nil;
    NSString *identifier = [plist isKindOfClass:NSDictionary.class] ? plist[@"CFBundleIdentifier"] : nil;
    NSString *executable = [plist isKindOfClass:NSDictionary.class] ? plist[@"CFBundleExecutable"] : nil;
    if (!*error &&
        (![identifier isKindOfClass:NSString.class] || !identifier.length || identifier.length > 255 ||
         ![executable isKindOfClass:NSString.class] || !executable.length ||
         ![executable isEqual:executable.lastPathComponent] || [executable isEqual:@"."] || [executable isEqual:@".."]))
        *error = EFTYPE;
    if (!*error) {
        const char *name = executable.fileSystemRepresentation;
        const char *identity = identifier.UTF8String;
        if (!name || strlen(name) != [executable lengthOfBytesUsingEncoding:NSUTF8StringEncoding] || !identity ||
            strlen(identity) != [identifier lengthOfBytesUsingEncoding:NSUTF8StringEncoding]) {
            close(bundle);
            *error = EFTYPE;
            return nil;
        }
        const int image = openat(bundle, name, flags);
        *error = image < 0 ? errno : switch_checked_file(image, NO, &metadata);
        if (!*error && !(metadata.st_mode & 0111))
            *error = EACCES;
        if (image >= 0)
            close(image);
    }
    close(bundle);
    return *error ? nil : @{@"Path" : path, @"CFBundleIdentifier" : identifier, @"CFBundleExecutable" : executable};
}

static NSArray<NSDictionary *> *switch_package_bundles(NSDictionary *app, int *error)
{
    NSString *plugins = [app[@"Path"] stringByAppendingPathComponent:@"PlugIns"];
    int directory = -1;
    *error = switch_open_directory(plugins, &directory);
    if (*error == ENOENT) {
        *error = 0;
        return @[ app ];
    }
    if (*error)
        return nil;
    close(directory);

    NSArray *names = [NSFileManager.defaultManager contentsOfDirectoryAtPath:plugins error:nil];
    if (!names) {
        *error = EIO;
        return nil;
    }
    NSMutableArray *bundles = [NSMutableArray arrayWithObject:app];
    NSMutableSet *identifiers = [NSMutableSet setWithObject:app[@"CFBundleIdentifier"]];
    for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
        if (![name.pathExtension isEqualToString:@"appex"])
            continue;
        NSDictionary *bundle = switch_bundle_info([plugins stringByAppendingPathComponent:name], error);
        if (!bundle)
            return nil;
        NSString *identifier = bundle[@"CFBundleIdentifier"];
        if ([identifiers containsObject:identifier]) {
            *error = EFTYPE;
            return nil;
        }
        [identifiers addObject:identifier];
        [bundles addObject:bundle];
    }
    return bundles;
}

static int switch_apply_app(NSString *path, NSString *oldIdentifier, BOOL remove)
{
    int error = 0;
    NSDictionary *app = remove ? nil : switch_bundle_info(path, &error);
    if (error == ENOENT) {
        struct stat metadata;
        // A deleted bundle is an unregister request; an incomplete bundle is an installation error.
        if (lstat(path.fileSystemRepresentation, &metadata) && errno == ENOENT) {
            error = 0;
            remove = YES;
        }
    }
    NSArray<NSDictionary *> *bundles = error || remove ? nil : switch_package_bundles(app, &error);
    if (error) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAppRegistration: invalid bundle %{public}@: %{public}s", path,
                     strerror(error));
        return EX_DATAERR;
    }
    NSString *identifier = app[@"CFBundleIdentifier"];
    if (identifier) {
        NSString *registered = switch_registered_app_path(identifier);
        if (registered && ![switch_app_path(registered) isEqual:path])
            return EX_NOPERM;
    }
    if (oldIdentifier && (remove || ![oldIdentifier isEqual:identifier])) {
        if (![switch_app_path(switch_registered_app_path(oldIdentifier)) isEqual:path])
            return EX_NOPERM;
        const int status = switch_apply_package_registration(path, oldIdentifier, nil);
        if (status)
            return status;
    }
    return remove ? EX_OK : switch_apply_package_registration(path, identifier, bundles);
}

int switch_registration_apps(NSArray<NSString *> *arguments, NSString **output)
{
    const BOOL migration = [arguments isEqual:@[ @"--finish-migration" ]];
    const BOOL sileo = migration || [arguments isEqual:@[ @"--register-sileo" ]];
    NSString *sileoPath = [switch_apps_root stringByAppendingPathComponent:@"Applications/Sileo.app"];
    if (sileo) {
        int error = 0;
        NSDictionary *app = switch_bundle_info(sileoPath, &error);
        if (!app)
            return error == ENOENT ? EX_NOINPUT : EX_DATAERR;
        if (![app[@"CFBundleIdentifier"] isEqual:@"org.coolstar.SileoStore"])
            return EX_DATAERR;
        arguments = migration ? @[ @"-a" ] : @[ @"-p", sileoPath ];
    }
    std::vector<const char *> argv = {"uicache"};
    for (NSString *argument in arguments)
        argv.push_back(argument.UTF8String);
    const SwitchUICacheOptions options = switch_uicache_parse((int)argv.size(), argv.data());
    if (!options.error.empty() || options.help || options.respring)
        return EX_USAGE;
    int root = -1;
    NSString *applications = [switch_apps_root stringByAppendingPathComponent:@"Applications"];
    const int error = switch_open_directory(applications, &root);
    if (error) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAppRegistration: invalid bootstrap apps: %{public}s", strerror(error));
        return EX_DATAERR;
    }
    close(root);
    NSError *nativeError = nil;
    if (!switch_app_registration_load(&nativeError)) {
        os_log_error(OS_LOG_DEFAULT, "SwitchAppRegistration: %{public}@", nativeError);
        return EX_UNAVAILABLE;
    }
    NSMutableDictionary<NSString *, NSString *> *registered = [NSMutableDictionary new];
    NSDictionary *installed = switch_registered_app_paths();
    for (NSString *path in installed) {
        NSString *canonical = switch_app_path(path);
        if (canonical)
            registered[canonical] = installed[path];
    }
    NSMutableOrderedSet<NSString *> *paths = [NSMutableOrderedSet new];
    NSMutableSet<NSString *> *remove = [NSMutableSet new];
    if (options.all) {
        NSArray *names = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:applications error:&nativeError];
        if (!names)
            return EX_IOERR;
        for (NSString *name in [names sortedArrayUsingSelector:@selector(compare:)]) {
            NSString *path = switch_app_path([applications stringByAppendingPathComponent:name]);
            if (path)
                [paths addObject:path];
        }
        [paths addObjectsFromArray:[registered.allKeys sortedArrayUsingSelector:@selector(compare:)]];
    }
    for (const std::string &argument : options.paths) {
        NSString *path = switch_app_path(@(argument.c_str()));
        if (!path)
            return EX_NOPERM;
        [paths addObject:path];
    }
    for (const std::string &argument : options.unregister) {
        NSString *path = switch_app_path(@(argument.c_str()));
        if (!path)
            return EX_NOPERM;
        [paths addObject:path];
        [remove addObject:path];
    }
    int status = EX_OK;
    for (NSString *path in paths) {
        const int result = switch_apply_app(path, registered[path], [remove containsObject:path]);
        if (result && !status)
            status = result;
    }
    if (options.list) {
        NSMutableString *text = [NSMutableString new];
        for (NSString *path in [registered.allKeys sortedArrayUsingSelector:@selector(compare:)])
            [text appendFormat:@"%@ %@\n", registered[path], path];
        *output = text;
    }
    if (sileo && !status) {
        NSString *identifier = @"org.coolstar.SileoStore";
        NSString *savedPath = switch_registered_app_path(identifier);
        if (![switch_app_path(savedPath) isEqual:sileoPath] || !switch_installed_record(savedPath, identifier, nil))
            return EX_TEMPFAIL;
    }
    return status;
}
