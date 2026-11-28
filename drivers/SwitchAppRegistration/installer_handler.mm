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
- (NSURL *)URL;
- (struct SwitchLSVersionNumber)_bundleVersion;
@end

@protocol SwitchClaimBinding <NSObject>
- (instancetype)initWithTypeIdentifier:(NSString *)identifier error:(NSError **)error;
- (id<SwitchContainerRecord>)bundleRecord;
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

static BOOL switch_ipa_handler_matches(NSString *path, NSString *identifier)
{
    Class bindingClass = NSClassFromString(@"LSClaimBinding");
    if (!bindingClass || ![bindingClass instancesRespondToSelector:@selector(initWithTypeIdentifier:error:)])
        return NO;
    NSError *error = nil;
    id<SwitchClaimBinding> binding = [(id<SwitchClaimBinding>)[bindingClass alloc]
        initWithTypeIdentifier:@"com.apple.itunes.ipa"
                         error:&error];
    id<SwitchContainerRecord> bundle =
        [binding respondsToSelector:@selector(bundleRecord)] ? [binding bundleRecord] : nil;
    if (![bundle respondsToSelector:@selector(bundleIdentifier)] ||
        ![bundle.bundleIdentifier isEqualToString:identifier])
        return NO;
    if ([bundle respondsToSelector:@selector(URL)] && bundle.URL.isFileURL) {
        NSString *actual = bundle.URL.URLByResolvingSymlinksInPath.path;
        NSString *expected = [NSURL fileURLWithPath:path isDirectory:YES].URLByResolvingSymlinksInPath.path;
        if (![actual isEqualToString:expected])
            return NO;
    }
    return YES;
}

static BOOL switch_register_installer_handler_at(NSString *path, NSString *identifier, NSError **error)
{
    if (![[NSFileManager defaultManager] fileExistsAtPath:path])
        return YES;
    if (!switch_app_registered_at_path(path, identifier))
        return switch_container_failure(error, @"Installer's saved record does not point to its built-in bundle");
    if (switch_ipa_handler_matches(path, identifier))
        return YES;
    id record = switch_installed_record(path, identifier, error);
    return record && switch_set_ipa_handler(record, error);
}

BOOL switch_register_installer_handler(NSError **error)
{
    return switch_register_installer_handler_at(@"/Applications/SwitchInstaller.app",
                                                @"com.brunostjohn.switch.installer", error);
}
