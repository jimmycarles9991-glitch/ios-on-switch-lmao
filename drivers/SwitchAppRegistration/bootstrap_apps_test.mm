// SPDX-License-Identifier: BSD-2-Clause-Patent
#import <Foundation/Foundation.h>
#include <assert.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/message.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xpc/xpc.h>

static ino_t switch_bad_owner;
static const char *switch_test_root;

static int switch_test_stat(int fd, struct stat *metadata, filesec_t security)
{
    const int result = fstatx_np(fd, metadata, security);
    if (!result) {
        metadata->st_uid = metadata->st_ino == switch_bad_owner ? 501 : 0;
        metadata->st_gid = 0;
        char path[PATH_MAX];
        assert(!fcntl(fd, F_GETPATH, path));
        if (strncmp(path, switch_test_root, strlen(switch_test_root))) {
            metadata->st_mode &= ~0022;
            filesec_unset_property(security, FILESEC_ACL);
        }
    }
    return result;
}

#define fstatx_np switch_test_stat
#include "bootstrap_apps.mm"
#undef fstatx_np

static NSMutableDictionary<NSString *, NSString *> *switch_test_installed;
static NSArray<NSDictionary *> *switch_test_registered_bundles;
static unsigned switch_test_calls;
static int switch_test_result;
static BOOL switch_test_placeholder;
static uid_t switch_test_effective_uid;
static audit_token_t switch_test_audit;
static xpc_object_t switch_test_reply;
static xpc_object_t switch_test_response;
static unsigned switch_test_cancellations;
static int switch_test_requirement_error;
static NSDictionary<NSString *, NSDictionary *> *switch_test_signatures;

extern "C" const CFStringRef switch_test_sign_entitlements = CFSTR("entitlements");
extern "C" const CFStringRef switch_test_sign_flags = CFSTR("flags");
extern "C" const CFStringRef switch_test_sign_identifier = CFSTR("identifier");
extern "C" const CFStringRef switch_test_sign_team = CFSTR("team");

extern "C" OSStatus switch_test_create_static_code(CFURLRef url, uint32_t, CFDictionaryRef, CFTypeRef *code)
{
    *code = CFRetain(url);
    return 0;
}

extern "C" OSStatus switch_test_signing_information(CFTypeRef code, uint32_t flags, CFDictionaryRef *information)
{
    assert(flags == 6);
    NSDictionary *signature = switch_test_signatures[((__bridge NSURL *)code).path];
    if (!signature)
        return -1;
    *information = (__bridge_retained CFDictionaryRef)signature;
    return 0;
}

static void *switch_test_open_mcm(const char *path, int)
{
    assert(strstr(path, "/MobileContainerManager.framework/MobileContainerManager"));
    return &switch_test_result;
}

static Class switch_test_container_class(NSString *name);

#define SecStaticCodeCreateWithPathAndAttributes switch_test_create_static_code
#define SecCodeCopySigningInformation            switch_test_signing_information
#define kSecCodeInfoEntitlementsDict             switch_test_sign_entitlements
#define kSecCodeInfoFlags                        switch_test_sign_flags
#define kSecCodeInfoIdentifier                   switch_test_sign_identifier
#define kSecCodeInfoTeamIdentifier               switch_test_sign_team
#define NSClassFromString                        switch_test_container_class
#define dlopen                                   switch_test_open_mcm
#include "package_records.mm"
#undef dlopen
#undef NSClassFromString
#undef kSecCodeInfoTeamIdentifier
#undef kSecCodeInfoIdentifier
#undef kSecCodeInfoFlags
#undef kSecCodeInfoEntitlementsDict
#undef SecCodeCopySigningInformation
#undef SecStaticCodeCreateWithPathAndAttributes

@interface SwitchTestContainer : NSObject <SwitchMCMContainer>
@property (nonatomic, strong) NSURL *url;
@end

@implementation SwitchTestContainer

+ (instancetype)containerWithIdentifier:(NSString *)identifier
                      createIfNecessary:(BOOL)create
                                existed:(BOOL *)existed
                                  error:(NSError **)error
{
    (void)existed;
    (void)error;
    assert(create);
    SwitchTestContainer *container = [self new];
    container.url = [NSURL
        fileURLWithPath:[NSString stringWithFormat:@"/var/containers/%@/%@", NSStringFromClass(self), identifier]];
    return container;
}

@end

@interface SwitchTestAppContainer : SwitchTestContainer
@end
@implementation SwitchTestAppContainer
@end
@interface SwitchTestPluginContainer : SwitchTestContainer
@end
@implementation SwitchTestPluginContainer
@end
@interface SwitchTestAppGroupContainer : SwitchTestContainer
@end
@implementation SwitchTestAppGroupContainer
@end
@interface SwitchTestSystemGroupContainer : SwitchTestContainer
@end
@implementation SwitchTestSystemGroupContainer
@end

static Class switch_test_container_class(NSString *name)
{
    return (@{
        @"MCMAppDataContainer" : SwitchTestAppContainer.class,
        @"MCMPluginKitPluginDataContainer" : SwitchTestPluginContainer.class,
        @"MCMSharedDataContainer" : SwitchTestAppGroupContainer.class,
        @"MCMSystemDataContainer" : SwitchTestSystemGroupContainer.class,
    })[name];
}

static uid_t switch_test_euid(void)
{
    return switch_test_effective_uid;
}

extern "C" void switch_test_audit_token(xpc_object_t, audit_token_t *token)
{
    *token = switch_test_audit;
}

static xpc_object_t switch_test_create_reply(xpc_object_t)
{
    return xpc_dictionary_create(NULL, NULL, 0);
}

static void switch_test_reply_to(xpc_connection_t, xpc_object_t reply)
{
    switch_test_reply = reply;
}

static int switch_test_requirement(xpc_connection_t, const char *entitlement, xpc_object_t value)
{
    assert(!strcmp(entitlement, "com.apple.private.coreservices.can-register-install-results"));
    assert(value == XPC_BOOL_TRUE);
    return switch_test_requirement_error;
}

static void switch_test_activate(xpc_connection_t)
{}

static void switch_test_event_handler(xpc_connection_t, xpc_handler_t)
{}

static void switch_test_cancel(xpc_connection_t)
{
    ++switch_test_cancellations;
}

static long switch_test_wait(dispatch_semaphore_t semaphore, dispatch_time_t deadline)
{
    assert(deadline >= dispatch_time(DISPATCH_TIME_NOW, 170 * NSEC_PER_SEC));
    assert(deadline <= dispatch_time(DISPATCH_TIME_NOW, 180 * NSEC_PER_SEC));
    return dispatch_semaphore_wait(semaphore, DISPATCH_TIME_NOW);
}

static void switch_test_request_to(xpc_connection_t, xpc_object_t message, dispatch_queue_t, xpc_handler_t handler);

#define geteuid                                                       switch_test_euid
#define xpc_dictionary_get_audit_token                                switch_test_audit_token
#define xpc_dictionary_create_reply                                   switch_test_create_reply
#define xpc_connection_send_message                                   switch_test_reply_to
#define xpc_connection_set_peer_entitlement_matches_value_requirement switch_test_requirement
#define xpc_connection_activate                                       switch_test_activate
#define xpc_connection_set_event_handler                              switch_test_event_handler
#define xpc_connection_cancel                                         switch_test_cancel
#define xpc_connection_send_message_with_reply                        switch_test_request_to
#define dispatch_semaphore_wait                                       switch_test_wait
#include "bootstrap_registration.mm"
#undef geteuid
#undef xpc_dictionary_get_audit_token
#undef xpc_dictionary_create_reply
#undef xpc_connection_send_message
#undef xpc_connection_set_peer_entitlement_matches_value_requirement
#undef xpc_connection_activate
#undef xpc_connection_set_event_handler
#undef xpc_connection_cancel
#undef xpc_connection_send_message_with_reply
#undef dispatch_semaphore_wait

static void switch_test_request_to(xpc_connection_t, xpc_object_t message, dispatch_queue_t, xpc_handler_t handler)
{
    assert(([switch_registration_arguments(message) isEqual:@[ @"-l" ]]));
    if (switch_test_response)
        handler(switch_test_response);
}

BOOL switch_app_registration_load(NSError **)
{
    return YES;
}

NSDictionary<NSString *, NSString *> *switch_registered_app_paths(void)
{
    return [switch_test_installed copy];
}

NSString *switch_registered_app_path(NSString *identifier)
{
    return [switch_test_installed allKeysForObject:identifier].firstObject;
}

id switch_installed_record(NSString *path, NSString *identifier, NSError **)
{
    return !switch_test_placeholder && [switch_test_installed[path] isEqual:identifier] ? identifier : nil;
}

int switch_apply_package_registration(NSString *path, NSString *identifier, NSArray<NSDictionary *> *bundles)
{
    ++switch_test_calls;
    if (switch_test_result)
        return switch_test_result;
    if (!bundles)
        [switch_test_installed removeObjectForKey:path];
    else {
        switch_test_installed[path] = identifier;
        switch_test_registered_bundles = bundles;
    }
    return 0;
}

static void switch_test_bundle(NSString *path, NSString *identifier)
{
    assert([[NSFileManager defaultManager] createDirectoryAtPath:path
                                     withIntermediateDirectories:YES
                                                      attributes:@{
                                                          NSFilePosixPermissions : @0755
                                                      }
                                                           error:nil]);
    NSDictionary *info = @{@"CFBundleIdentifier" : identifier, @"CFBundleExecutable" : @"App"};
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:info
                                                              format:NSPropertyListBinaryFormat_v1_0
                                                             options:0
                                                               error:nil];
    NSString *executable = [path stringByAppendingPathComponent:@"App"];
    assert([data writeToFile:[path stringByAppendingPathComponent:@"Info.plist"] atomically:YES]);
    assert([[NSData data] writeToFile:executable atomically:YES]);
    assert(!chmod(executable.fileSystemRepresentation, 0755));
}

static void switch_test_bundle_info(NSString *app)
{
    NSString *output = nil;
    NSString *info = [app stringByAppendingPathComponent:@"Info.plist"];
    NSData *original = [NSData dataWithContentsOfFile:info];
    const unsigned calls = switch_test_calls;
    for (id bad in @[
             @[], @{},
             @{@"CFBundleIdentifier" : @4,
               @"CFBundleExecutable" : @"App"},
             @{@"CFBundleIdentifier" : @"example.valid",
               @"CFBundleExecutable" : @42},
             @{@"CFBundleIdentifier" : @"", @"CFBundleExecutable" : @"App"},
             @{@"CFBundleIdentifier" : @"example.valid", @"CFBundleExecutable" : @"../App"}
         ]) {
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:bad
                                                                  format:NSPropertyListBinaryFormat_v1_0
                                                                 options:0
                                                                   error:nil];
        assert([data writeToFile:info atomically:YES]);
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    }
    assert([original writeToFile:info atomically:YES]);
    NSString *backup = [app stringByAppendingPathComponent:@"Info.saved"];
    assert(!rename(info.fileSystemRepresentation, backup.fileSystemRepresentation));
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert(!symlink("Info.saved", info.fileSystemRepresentation));
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert(!unlink(info.fileSystemRepresentation));
    assert(!link(backup.fileSystemRepresentation, info.fileSystemRepresentation));
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert(!unlink(info.fileSystemRepresentation));
    assert(!rename(backup.fileSystemRepresentation, info.fileSystemRepresentation));
    NSString *executable = [app stringByAppendingPathComponent:@"App"];
    assert(!chmod(executable.fileSystemRepresentation, 0644));
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert(!chmod(executable.fileSystemRepresentation, 0755));
    assert(switch_test_calls == calls);
    assert([switch_test_installed[app] isEqual:@"org.coolstar.SileoStore"]);
}

static void switch_test_xpc(void)
{
    xpc_object_t message = xpc_dictionary_create(NULL, NULL, 0);
    xpc_object_t arguments = xpc_array_create(NULL, 0);
    xpc_array_set_string(arguments, XPC_ARRAY_APPEND, "-l");
    xpc_dictionary_set_value(message, "arguments", arguments);
    assert(([switch_registration_arguments(message) isEqual:@[ @"-l" ]]));

    switch_test_audit.val[5] = 72;
    for (const uid_t uid : {0u, 501u, 502u}) {
        switch_test_audit.val[1] = uid;
        switch_registration_answer(nil, message);
        assert(xpc_dictionary_get_int64(switch_test_reply, "status") == (uid == 502 ? EX_NOPERM : EX_OK));
    }
    switch_test_audit.val[5] = 0;
    switch_test_audit.val[1] = 0;
    switch_registration_answer(nil, message);
    assert(xpc_dictionary_get_int64(switch_test_reply, "status") == EX_NOPERM);
    switch_test_audit.val[5] = 72;
    xpc_dictionary_set_bool(message, "extra", true);
    switch_registration_answer(nil, message);
    assert(xpc_dictionary_get_int64(switch_test_reply, "status") == EX_USAGE);
    xpc_dictionary_set_value(message, "extra", NULL);
    xpc_dictionary_set_string(message, "arguments", "-l");
    assert(!switch_registration_arguments(message));
    xpc_dictionary_set_value(message, "arguments", xpc_array_create(NULL, 0));
    assert(!switch_registration_arguments(message));
    xpc_dictionary_set_value(message, "arguments", arguments);
    xpc_array_set_int64(arguments, 0, 42);
    assert(!switch_registration_arguments(message));
    std::string longArgument(PATH_MAX, 'x');
    xpc_array_set_string(arguments, 0, longArgument.c_str());
    assert(!switch_registration_arguments(message));
    for (size_t index = 0; index < 256; ++index)
        xpc_array_set_string(arguments, XPC_ARRAY_APPEND, "-l");
    assert(!switch_registration_arguments(message));

    switch_test_effective_uid = 502;
    assert(switch_registration_request(@[ @"-l" ]) == EX_NOPERM);
    assert(!switch_test_cancellations);
    switch_test_effective_uid = 501;
    switch_test_requirement_error = EPERM;
    assert(switch_registration_request(@[ @"-l" ]) == EX_NOPERM);
    assert(switch_test_cancellations == 1);
    switch_test_requirement_error = 0;
    assert(switch_registration_request(@[ @"-l" ]) == EX_TEMPFAIL);
    assert(switch_test_cancellations == 2);
    switch_test_response = xpc_dictionary_create(NULL, NULL, 0);
    switch_test_effective_uid = 0;
    assert(switch_registration_request(@[ @"-l" ]) == EX_NOPERM);
    switch_test_audit.val[1] = 501;
    xpc_dictionary_set_bool(switch_test_response, "status", false);
    assert(switch_registration_request(@[ @"-l" ]) == EX_PROTOCOL);
    xpc_dictionary_set_int64(switch_test_response, "status", EX_OK);
    assert(!switch_registration_request(@[ @"-l" ]));
}

static void switch_test_plugins(NSString *app)
{
    NSString *plugins = [app stringByAppendingPathComponent:@"PlugIns"];
    NSString *plugin = [plugins stringByAppendingPathComponent:@"Widget.appex"];
    switch_test_bundle(plugin, @"org.coolstar.SileoStore.Widget");
    NSString *output = nil;
    assert(!switch_registration_apps(@[ @"-p", app ], &output));
    assert(switch_test_registered_bundles.count == 2);
    assert([switch_test_registered_bundles[0][@"Path"] isEqual:app]);
    assert([switch_test_registered_bundles[1][@"Path"] isEqual:plugin]);
    assert([switch_test_registered_bundles[1][@"CFBundleIdentifier"] isEqual:@"org.coolstar.SileoStore.Widget"]);
    assert([switch_test_registered_bundles[1][@"CFBundleExecutable"] isEqual:@"App"]);

    switch_test_signatures = @{
        [app stringByAppendingPathComponent:@"App"] : @{
            @"identifier" : @"org.coolstar.SileoStore",
            @"flags" : @2,
            @"entitlements" : @{@"com.apple.private.security.no-container" : @YES},
        },
        [plugin stringByAppendingPathComponent:@"App"] : @{
            @"identifier" : @"org.coolstar.SileoStore.Widget",
            @"team" : @"ExampleTeam",
            @"entitlements" : @{
                @"com.apple.private.security.container-required" : @"org.coolstar.SileoStore.WidgetData",
                @"com.apple.security.application-groups" : @[ @"group.example.shared" ],
                @"com.apple.security.system-groups" : @[ @"systemgroup.example.shared" ],
            },
        },
    };
    NSError *error = nil;
    NSArray<NSDictionary *> *records = switch_package_install_info(switch_test_registered_bundles, &error);
    assert(records.count == 2 && !error);
    assert([records[0][@"ApplicationType"] isEqual:@"System"] && [records[0][@"IsAdHocSigned"] boolValue]);
    assert(!records[0][@"SignerIdentity"] && !records[0][@"SignerOrganization"]);
    assert([records[0][@"Container"] hasSuffix:@"/SwitchTestAppContainer/org.coolstar.SileoStore"]);
    assert(![records[0][@"IsContainerized"] boolValue] &&
           [records[0][@"EnvironmentVariables"][@"HOME"] isEqual:@"/var/mobile"]);
    assert([records[1][@"ApplicationType"] isEqual:@"PluginKitPlugin"] &&
           [records[1][@"PluginOwnerBundleID"] isEqual:@"org.coolstar.SileoStore"]);
    assert([records[1][@"Container"] hasSuffix:@"/SwitchTestPluginContainer/org.coolstar.SileoStore.WidgetData"]);
    assert([records[1][@"GroupContainers"][@"group.example.shared"] containsString:@"/SwitchTestAppGroupContainer/"]);
    assert([records[1][@"GroupContainers"][@"systemgroup.example.shared"]
        containsString:@"/SwitchTestSystemGroupContainer/"]);
    assert([records[1][@"TeamIdentifier"] isEqual:@"ExampleTeam"]);
    switch_test_signatures = @{};
    assert(!switch_package_install_info(switch_test_registered_bundles, &error) && error);

    const unsigned calls = switch_test_calls;
    NSString *duplicate = [plugins stringByAppendingPathComponent:@"Duplicate.appex"];
    switch_test_bundle(duplicate, @"org.coolstar.SileoStore.Widget");
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert([NSFileManager.defaultManager removeItemAtPath:duplicate error:nil]);
    assert(!symlink("Widget.appex", duplicate.fileSystemRepresentation));
    assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
    assert(switch_test_calls == calls);
    assert([NSFileManager.defaultManager removeItemAtPath:plugins error:nil]);
}

int main()
{
    @autoreleasepool {
        char temporary[] = "/private/tmp/switch-package-apps-XXXXXX";
        assert(mkdtemp(temporary));
        switch_test_root = temporary;
        switch_apps_root = @(temporary);
        switch_test_installed = [NSMutableDictionary new];
        NSString *app = [switch_apps_root stringByAppendingPathComponent:@"Applications/Sileo.app"];
        NSString *other = [switch_apps_root stringByAppendingPathComponent:@"Applications/PreferenceApp.app"];
        switch_test_bundle(app, @"org.coolstar.SileoStore");
        switch_test_bundle(other, @"example.preferences");
        NSString *output = nil;
        assert(!switch_registration_apps(@[ @"-a" ], &output));
        assert(switch_test_installed.count == 2 && switch_test_calls == 2);
        assert(!switch_registration_apps(@[ @"-p", @"/var/jb/Applications/Sileo.app" ], &output));
        assert(switch_test_calls == 3);
        assert(!switch_registration_apps(@[ @"--register-sileo" ], &output));
        assert(!switch_registration_apps(@[ @"--finish-migration" ], &output));
        assert([switch_test_installed[app] isEqual:@"org.coolstar.SileoStore"]);
        assert([switch_test_installed[other] isEqual:@"example.preferences"]);
        switch_test_installed[@"/Applications/Unrelated.app"] = @"example.unrelated";
        NSDictionary *complete = [switch_test_installed copy];
        [switch_test_installed removeObjectForKey:other];
        assert(!switch_registration_apps(@[ @"--finish-migration" ], &output));
        assert([switch_test_installed isEqualToDictionary:complete]);
        switch_test_placeholder = YES;
        assert(switch_registration_apps(@[ @"--finish-migration" ], &output) == EX_TEMPFAIL);
        switch_test_placeholder = NO;
        switch_test_result = EX_TEMPFAIL;
        assert(switch_registration_apps(@[ @"--register-sileo" ], &output) == EX_TEMPFAIL);
        switch_test_result = 0;
        assert(!switch_registration_apps(@[ @"-l" ], &output));
        assert([output containsString:@"org.coolstar.SileoStore"]);
        [switch_test_installed removeAllObjects];
        output = nil;
        assert(!switch_registration_apps(@[ @"-a", @"-l" ], &output));
        assert([output containsString:@"org.coolstar.SileoStore"]);
        assert([output containsString:@"example.preferences"]);
        output = nil;
        assert(!switch_registration_apps(@[ @"-u", other, @"-l" ], &output));
        assert([output containsString:@"org.coolstar.SileoStore"]);
        assert(![output containsString:@"example.preferences"]);
        assert(!switch_registration_apps(@[ @"-p", other ], &output));
        switch_test_bundle_info(app);
        switch_test_xpc();
        switch_test_plugins(app);
        assert(!switch_registration_apps(@[ @"-u", other ], &output));
        assert(!switch_test_installed[other]);
        assert(!switch_registration_apps(@[ @"-p", other ], &output));
        assert([[NSFileManager defaultManager] removeItemAtPath:other error:nil]);
        assert(!switch_registration_apps(@[ @"-p", other ], &output));
        assert(!switch_test_installed[other]);
        switch_test_installed[other] = @"example.preferences";
        assert(!switch_registration_apps(@[ @"-a" ], &output));
        assert(!switch_test_installed[other]);
        const unsigned calls = switch_test_calls;
        for (NSString *bad in @[
                 @"/Applications/Settings.app", @"/var/jb/Applications/../Bad.app",
                 @"/var/jb/Applications/Bad.app/Other.app"
             ])
            assert(switch_registration_apps(@[ @"-p", bad ], &output) == EX_NOPERM);
        assert(switch_test_calls == calls);
        NSString *link = [switch_apps_root stringByAppendingPathComponent:@"Applications/Linked.app"];
        assert(!symlink(app.fileSystemRepresentation, link.fileSystemRepresentation));
        assert(switch_registration_apps(@[ @"-p", link ], &output) == EX_DATAERR);
        assert(!unlink(link.fileSystemRepresentation));
        NSString *executable = [app stringByAppendingPathComponent:@"App"];
        struct stat metadata;
        assert(!lstat(executable.fileSystemRepresentation, &metadata));
        switch_bad_owner = metadata.st_ino;
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
        switch_bad_owner = 0;
        assert(!chmod(executable.fileSystemRepresentation, 0777));
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
        assert(!chmod(executable.fileSystemRepresentation, 0755));
        NSString *hardlink = [switch_apps_root stringByAppendingPathComponent:@"linked-image"];
        assert(!::link(executable.fileSystemRepresentation, hardlink.fileSystemRepresentation));
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
        assert(!unlink(hardlink.fileSystemRepresentation));
        [switch_test_installed removeObjectForKey:app];
        switch_test_installed[@"/Applications/Settings.app"] = @"org.coolstar.SileoStore";
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_NOPERM);
        assert(switch_test_calls == calls);
        [switch_test_installed removeAllObjects];
        switch_test_result = EX_TEMPFAIL;
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_TEMPFAIL);
        assert(!switch_test_installed.count);
        switch_test_result = 0;
        assert([[NSFileManager defaultManager] removeItemAtPath:executable error:nil]);
        assert(switch_registration_apps(@[ @"-p", app ], &output) == EX_DATAERR);
        assert([[NSFileManager defaultManager] removeItemAtPath:switch_apps_root error:nil]);
    }
}
