#import "SwitchImportDiagnostics.h"

#if DEVELOPMENT || DEBUG
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <os/log.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int proc_pidpath(int pid, void *buffer, uint32_t size);
extern "C" int proc_pidpath_audittoken(audit_token_t *token, void *buffer, uint32_t size);

@protocol SwitchImportRecord <NSObject>
+ (id)bundleRecordForAuditToken:(audit_token_t *)token error:(NSError **)error;
+ (id)_bundleRecordForAuditToken:(audit_token_t *)token checkNSBundleMainBundle:(BOOL)check error:(NSError **)error;
+ (id)bundleRecordWithBundleIdentifier:(NSString *)identifier
                      allowPlaceholder:(BOOL)placeholder
                                 error:(NSError **)error;
- (id)initWithURL:(NSURL *)url allowPlaceholder:(BOOL)placeholder error:(NSError **)error;
- (NSString *)bundleIdentifier;
- (NSData *)installSessionIdentifier;
- (NSURL *)URL;
@end

@protocol SwitchImportNode <NSObject>
- (instancetype)initWithURL:(NSURL *)url flags:(NSUInteger)flags error:(NSError **)error;
- (BOOL)getVolumeIdentifier:(uint64_t *)identifier error:(NSError **)error;
- (BOOL)getFileIdentifier:(uint64_t *)identifier error:(NSError **)error;
@end

static NSDictionary *switch_record(id<SwitchImportRecord> record, NSError *error)
{
    NSMutableDictionary *result = [NSMutableDictionary new];
    if (record) {
        result[@"class"] = NSStringFromClass([record class]);
        result[@"description"] = [record description];
        if ([record respondsToSelector:@selector(bundleIdentifier)])
            result[@"identifier"] = record.bundleIdentifier;
        if ([record respondsToSelector:@selector(installSessionIdentifier)])
            result[@"session"] = record.installSessionIdentifier;
        if ([record respondsToSelector:@selector(URL)])
            result[@"url"] = record.URL.path;
    }
    if (error)
        result[@"error"] = @{@"domain" : error.domain, @"code" : @(error.code), @"message" : error.description};
    return result;
}

static NSDictionary *switch_file_identity(NSString *path)
{
    NSMutableDictionary *result = [NSMutableDictionary new];
    struct stat file = {};
    if (stat(path.fileSystemRepresentation, &file)) {
        result[@"statError"] = @(errno);
        return result;
    }
    result[@"device"] = @(file.st_dev);
    result[@"inode"] = @(file.st_ino);
    Class nodes = NSClassFromString(@"FSNode");
    if ([nodes instancesRespondToSelector:@selector(initWithURL:flags:error:)]) {
        NSError *error = nil;
        id<SwitchImportNode> node = [[nodes alloc] initWithURL:[NSURL fileURLWithPath:path] flags:0 error:&error];
        uint64_t identifier = 0;
        if (node && [node respondsToSelector:@selector(getVolumeIdentifier:error:)] &&
            [node getVolumeIdentifier:&identifier error:&error])
            result[@"nodeVolume"] = @(identifier);
        if (node && [node respondsToSelector:@selector(getFileIdentifier:error:)] &&
            [node getFileIdentifier:&identifier error:&error])
            result[@"nodeFile"] = @(identifier);
        if (error)
            result[@"nodeError"] = error.description;
    }
    struct statfs volume = {};
    if (!statfs(path.fileSystemRepresentation, &volume)) {
        result[@"statfsID"] = @[ @(volume.f_fsid.val[0]), @(volume.f_fsid.val[1]) ];
        result[@"statfsMount"] = @(volume.f_mntonname);
        struct stat root = {};
        if (!stat(volume.f_mntonname, &root))
            result[@"statfsRootDevice"] = @(root.st_dev);
    }
    using GetPath = ssize_t (*)(void *, size_t, fsid_t *, uint64_t);
    const auto getPath = reinterpret_cast<GetPath>(dlsym(RTLD_DEFAULT, "fsgetpath"));
    struct statfs *mounts = nullptr;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int index = 0; index < count; ++index) {
        if (mounts[index].f_fsid.val[0] != file.st_dev)
            continue;
        result[@"volumeMount"] = @(mounts[index].f_mntonname);
        if (getPath) {
            char resolved[PATH_MAX] = {};
            if (getPath(resolved, sizeof(resolved), &mounts[index].f_fsid, file.st_ino) > 0)
                result[@"pathFromID"] = @(resolved);
            else
                result[@"pathFromIDError"] = @(errno);
        }
        break;
    }
    return result;
}

static NSDictionary *switch_import_identity(NSString *event)
{
    if (!NSClassFromString(@"LSBundleRecord"))
        dlopen("/System/Library/Frameworks/CoreServices.framework/CoreServices", RTLD_NOW);
    NSBundle *bundle = NSBundle.mainBundle;
    NSMutableDictionary *report = [@{
        @"event" : event,
        @"time" : NSDate.date,
        @"pid" : @(getpid()),
        @"build" : [bundle objectForInfoDictionaryKey:@"CFBundleVersion"] ?: @"",
        @"identifier" : bundle.bundleIdentifier ?: @"",
        @"bundle" : bundle.bundlePath,
        @"executable" : bundle.executablePath ?: @"",
        @"bundleIdentity" : switch_file_identity(bundle.bundlePath),
        @"executableIdentity" : switch_file_identity(bundle.executablePath ?: @""),
    } mutableCopy];
    char path[PATH_MAX] = {};
    if (proc_pidpath(getpid(), path, sizeof(path)) > 0)
        report[@"processPath"] = @(path);
    else
        report[@"processPathError"] = @(errno);
    audit_token_t token = {};
    mach_msg_type_number_t count = TASK_AUDIT_TOKEN_COUNT;
    const kern_return_t status =
        task_info(mach_task_self(), TASK_AUDIT_TOKEN, reinterpret_cast<task_info_t>(&token), &count);
    report[@"auditStatus"] = @(status);
    if (status != KERN_SUCCESS)
        return report;
    if (proc_pidpath_audittoken(&token, path, sizeof(path)) > 0)
        report[@"auditedProcessPath"] = @(path);
    else
        report[@"auditedProcessPathError"] = @(errno);
    Class<SwitchImportRecord> records = NSClassFromString(@"LSBundleRecord");
    Class applications = NSClassFromString(@"LSApplicationRecord");
    NSError *error = nil;
    id record = [records bundleRecordWithBundleIdentifier:bundle.bundleIdentifier allowPlaceholder:NO error:&error];
    report[@"byIdentifier"] = switch_record(record, error);
    error = nil;
    record = [[applications alloc] initWithURL:bundle.bundleURL allowPlaceholder:NO error:&error];
    report[@"byURL"] = switch_record(record, error);
    error = nil;
    record = [records bundleRecordForAuditToken:&token error:&error];
    report[@"byAudit"] = switch_record(record, error);
    error = nil;
    record = [records _bundleRecordForAuditToken:&token checkNSBundleMainBundle:NO error:&error];
    report[@"byExternalAudit"] = switch_record(record, error);
    void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_NOW);
    using CreateTask = CFTypeRef (*)(CFAllocatorRef);
    using CopyEntitlement = CFTypeRef (*)(CFTypeRef, CFStringRef, CFErrorRef *);
    const auto createTask = security ? reinterpret_cast<CreateTask>(dlsym(security, "SecTaskCreateFromSelf")) : nullptr;
    const auto copyEntitlement =
        security ? reinterpret_cast<CopyEntitlement>(dlsym(security, "SecTaskCopyValueForEntitlement")) : nullptr;
    if (createTask && copyEntitlement) {
        CFTypeRef task = createTask(kCFAllocatorDefault);
        if (task) {
            CFTypeRef identity = copyEntitlement(task, CFSTR("application-identifier"), nullptr);
            if (identity)
                report[@"runtimeApplicationIdentifier"] = CFBridgingRelease(identity);
            CFRelease(task);
        }
    }
    if (security)
        dlclose(security);
    return report;
}

@implementation SwitchImportDiagnostics

+ (void)capture:(NSString *)event
{
    static dispatch_queue_t queue;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        queue = dispatch_queue_create("com.brunostjohn.switch.installer.import-diagnostics", DISPATCH_QUEUE_SERIAL);
    });
    dispatch_async(queue, ^{
        @autoreleasepool {
            NSDictionary *report = switch_import_identity(event);
            NSURL *directory = [[[NSFileManager defaultManager] URLsForDirectory:NSApplicationSupportDirectory
                                                                       inDomains:NSUserDomainMask]
                                    .firstObject URLByAppendingPathComponent:@"SwitchInstaller"
                                                                 isDirectory:YES];
            NSURL *destination = [directory URLByAppendingPathComponent:@"import-diagnostics.plist"];
            NSMutableArray *history =
                [[NSArray arrayWithContentsOfURL:destination] mutableCopy] ?: [NSMutableArray new];
            [history addObject:report];
            while (history.count > 16)
                [history removeObjectAtIndex:0];
            NSError *error = nil;
            NSData *data = [NSPropertyListSerialization dataWithPropertyList:history
                                                                      format:NSPropertyListBinaryFormat_v1_0
                                                                     options:0
                                                                       error:&error];
            BOOL saved = data &&
                         [[NSFileManager defaultManager] createDirectoryAtURL:directory
                                                  withIntermediateDirectories:YES
                                                                   attributes:nil
                                                                        error:&error] &&
                         [data writeToURL:destination
                                  options:NSDataWritingAtomic | NSDataWritingFileProtectionNone
                                    error:&error];
            os_log_with_type(OS_LOG_DEFAULT, saved ? OS_LOG_TYPE_DEFAULT : OS_LOG_TYPE_ERROR,
                             "SwitchImportDiagnostics: %{public}@ saved=%d %{public}@", event, saved, error);
        }
    });
}

@end
#endif
