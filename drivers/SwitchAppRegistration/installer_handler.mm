#import "installer_handler.h"
#import "app_registration.h"

@protocol SwitchContainerWorkspace <NSObject>
+ (instancetype)defaultWorkspace;
- (void)setDefaultHandlerForTypeRecord:(id)type
                   toApplicationRecord:(id)application
                     completionHandler:(void (^)(BOOL, NSError *))completion;
@end

struct SwitchLSVersionNumber {
    unsigned char bytes[32];
};

@protocol SwitchContainerRecord <NSObject>
- (instancetype)initWithURL:(NSURL *)url allowPlaceholder:(BOOL)placeholder error:(NSError **)error;
- (NSString *)bundleIdentifier;
- (struct SwitchLSVersionNumber)_bundleVersion;
@end

@protocol SwitchContainerType <NSObject>
+ (instancetype)typeRecordWithIdentifier:(NSString *)identifier;
@end

@protocol SwitchHandlerService <NSObject>
+ (id)XPCProxyWithErrorHandler:(void (^)(NSError *))handler;
- (void)setHandler:(NSString *)identifier
              version:(struct SwitchLSVersionNumber)version
                roles:(uint32_t)roles
       forContentType:(NSString *)type
    completionHandler:(void (^)(BOOL, NSError *))completion;
@end

static BOOL switch_container_failure(NSError **error, NSString *reason)
{
    if (error)
        *error = [NSError errorWithDomain:@"com.brunostjohn.switch.appregd"
                                     code:2
                                 userInfo:@{NSLocalizedDescriptionKey : reason}];
    return NO;
}

static BOOL switch_set_ipa_handler(id<SwitchContainerRecord> record, NSError **error)
{
    NSString *identifier = @"com.apple.itunes.ipa";
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    __block BOOL succeeded = NO;
    __block NSError *failure = nil;
    void (^completion)(BOOL, NSError *) = ^(BOOL success, NSError *resultError) {
        succeeded = success && !resultError;
        failure = resultError;
        dispatch_semaphore_signal(finished);
    };
#if SWITCH_APPREGD_INSTALLATION_RECORDS
    Class<SwitchContainerWorkspace> workspaceType = NSClassFromString(@"LSApplicationWorkspace");
    id<SwitchContainerWorkspace> workspace = [workspaceType defaultWorkspace];
    Class<SwitchContainerType> typeClass = NSClassFromString(@"UTTypeRecord");
    if (![workspace
            respondsToSelector:@selector(setDefaultHandlerForTypeRecord:toApplicationRecord:completionHandler:)] ||
        ![typeClass respondsToSelector:@selector(typeRecordWithIdentifier:)])
        return switch_container_failure(error, @"LaunchServices default document handler API unavailable");
    id type = [typeClass typeRecordWithIdentifier:identifier];
    if (!type)
        return switch_container_failure(error, @"IPA document type is not registered");
    [workspace setDefaultHandlerForTypeRecord:type toApplicationRecord:record completionHandler:completion];
#else
    Class<SwitchHandlerService> service = NSClassFromString(@"_LSDModifyService");
    if (![service respondsToSelector:@selector(XPCProxyWithErrorHandler:)] ||
        ![record respondsToSelector:@selector(_bundleVersion)])
        return switch_container_failure(error, @"LaunchServices document handler service unavailable");
    id<SwitchHandlerService> proxy = [service XPCProxyWithErrorHandler:^(NSError *resultError) {
        completion(NO, resultError);
    }];
    [proxy setHandler:record.bundleIdentifier
                  version:record._bundleVersion
                    roles:UINT32_MAX
           forContentType:identifier
        completionHandler:completion];
#endif
    if (dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_SEC)))
        return switch_container_failure(error, @"LaunchServices did not finish setting the IPA handler");
    if (!succeeded) {
        if (failure && error)
            *error = failure;
        else
            switch_container_failure(error, @"LaunchServices rejected the IPA handler");
    }
    return succeeded;
}

id switch_installed_record(NSString *path, NSString *identifier, NSError **error)
{
    Class recordClass = NSClassFromString(@"LSApplicationRecord");
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    id<SwitchContainerRecord> record = [(id<SwitchContainerRecord>)[recordClass alloc] initWithURL:url
                                                                                  allowPlaceholder:NO
                                                                                             error:error];
    if (!record)
        return nil;
    if (![record.bundleIdentifier isEqualToString:identifier]) {
        switch_container_failure(error, @"Unexpected bundle identity in LaunchServices record");
        return nil;
    }
    return record;
}

BOOL switch_register_installer_handler(NSError **error)
{
    NSString *path = @"/Applications/SwitchInstaller.app";
    NSString *identifier = @"com.brunostjohn.switch.installer";
    if (![[NSFileManager defaultManager] fileExistsAtPath:path])
        return YES;
    if (!switch_app_registered_at_path(path, identifier))
        return switch_container_failure(error, @"Installer's saved record does not point to its built-in bundle");
    id record = switch_installed_record(path, identifier, error);
    return record && switch_set_ipa_handler(record, error);
}
