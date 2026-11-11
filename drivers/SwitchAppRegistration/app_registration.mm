// SPDX-License-Identifier: BSD-2-Clause-Patent
// What installd does for a built-in app (21H461 and 24B119 installd):
//   register    -[MILaunchServicesOperationManager registerInstalledInfoForBuiltInAppAtURL:error:]: MIExecutableBundle
//               of the URL, +[MILaunchServicesDatabaseGatherer fetchInfoForBundle:...] (the app first, then its
//               extensions; it also creates the app's data container), then LaunchServices, with a request context
//               for user 501. 21H461 hands LaunchServices the records as info dictionaries
//               (registerContainerizedApplicationWithInfoDictionaries:), 24B119 as installation records.
//   unregister  21H461 has no built-in removal of its own (its migration rebuilds the database); its uninstall
//               unregisters the bundle identifier with unregistrationOperation 3. 24B119 removes an app on the root by
//               URL (unregisterBundleOnRootAtURL: -> unregisterBuiltinApplicationAtURL:).
// Each LaunchServices call is retried while lsd's connection is interrupted or invalid, at most 5 times a second apart
// (installd's MIRetrySynchronousIPC). lsd accepts these operations only from a client holding
// com.apple.private.coreservices.can-register-install-results (-[_LSDModifyClient
// clientIsEntitledForPostInstallationOperations], 24B119's clientIsEntitledForEmbeddedRegistrationOperations).
#import "app_registration.h"

#include <dlfcn.h>
#include <unistd.h>

@protocol SwitchLSApplicationWorkspace <NSObject>
+ (id)defaultWorkspace;
- (id)registerContainerizedApplicationWithInfoDictionaries:(NSArray *)infos
                                             operationUUID:(NSUUID *)uuid
                                            requestContext:(id)context
                                              saveObserver:(id<LSOperationSaveObserver>)observer
                                         registrationError:(NSError **)error;
- (id)registerContainerizedApplicationWithInstallationRecord:(id)record
                                extensionInstallationRecords:(NSArray *)extensions
                                        personaUniqueStrings:(NSArray *)personas
                                               operationUUID:(NSUUID *)uuid
                                              requestContext:(id)context
                                                saveObserver:(id<LSOperationSaveObserver>)observer
                                           registrationError:(NSError **)error;
- (BOOL)unregisterContainerizedApplicationWithBundleIdentifier:(NSString *)identifier
                                                 operationUUID:(NSUUID *)uuid
                                       unregistrationOperation:(unsigned int)operation
                                                  precondition:(id)precondition
                                                requestContext:(id)context
                                                  saveObserver:(id<LSOperationSaveObserver>)observer
                                           unregistrationError:(NSError **)error;
- (BOOL)unregisterBuiltinApplicationAtURL:(NSURL *)url
                            operationUUID:(NSUUID *)uuid
                           requestContext:(id)context
                             saveObserver:(id<LSOperationSaveObserver>)observer
                                    error:(NSError **)error;
@end

@protocol SwitchLSApplicationProxy <NSObject>
+ (id)applicationProxyForIdentifier:(NSString *)identifier;
- (BOOL)isInstalled;
@end

@protocol SwitchLSOperationRequestContext <NSObject>
- (void)setTargetUserID:(NSNumber *)uid;
@end

@protocol SwitchMIBundle <NSObject>
- (instancetype)initWithBundleURL:(NSURL *)url error:(NSError **)error;
@end

@protocol SwitchMILaunchServicesDatabaseGatherer <NSObject>
+ (NSArray *)fetchInfoForBundle:(id)bundle
                     forPersona:(NSString *)persona
                    inContainer:(id)container
                      withError:(NSError **)error;
+ (NSArray *)fetchInfoForBundle:(id)bundle
                    forPersonas:(NSSet *)personas
                    inContainer:(id)container
                      withError:(NSError **)error;
@end

@protocol SwitchICLBundleRecord <NSObject>
+ (NSArray *)bundleRecordArrayToInfoDictionaryArray:(NSArray *)records;
@end

static const unsigned kMobileUser = 501;
static const unsigned kIPCAttempts = 5;

static Class<SwitchLSApplicationWorkspace> workspaceClass;
static Class<SwitchLSApplicationProxy> proxyClass;
static Class contextClass;
static Class bundleClass;
static Class<SwitchMILaunchServicesDatabaseGatherer> gathererClass;
static Class<SwitchICLBundleRecord> recordClass;

static NSError *failure(NSString *what)
{
    return [NSError errorWithDomain:@"com.brunostjohn.switch.appregd"
                               code:1
                           userInfo:@{NSLocalizedDescriptionKey : what}];
}

static Class loaded(NSString *name, NSError **error)
{
    Class found = NSClassFromString(name);
    if (!found && error)
        *error = failure([name stringByAppendingString:@" not found"]);
    return found;
}

BOOL switch_app_registration_load(NSError **error)
{
    static const char *const frameworks[] = {
        "/System/Library/Frameworks/CoreServices.framework/CoreServices",
        "/System/Library/PrivateFrameworks/InstalledContentLibrary.framework/InstalledContentLibrary",
    };
    for (const char *framework : frameworks) {
        if (!dlopen(framework, RTLD_NOW)) {
            if (error)
                *error = failure(@(dlerror()));
            return NO;
        }
    }
    workspaceClass = loaded(@"LSApplicationWorkspace", error);
    proxyClass = loaded(@"LSApplicationProxy", error);
    contextClass = loaded(@"LSOperationRequestContext", error);
    bundleClass = loaded(@"MIExecutableBundle", error);
    gathererClass = loaded(@"MILaunchServicesDatabaseGatherer", error);
    recordClass = loaded(@"ICLBundleRecord", error);
    return workspaceClass && proxyClass && contextClass && bundleClass && gathererClass && recordClass;
}

BOOL switch_app_installed(NSString *identifier)
{
    return [[proxyClass applicationProxyForIdentifier:identifier] isInstalled];
}

static id requestContext(void)
{
    id<SwitchLSOperationRequestContext> context = [contextClass new];
    [context setTargetUserID:@(kMobileUser)];
    return context;
}

static BOOL retrying(BOOL (^call)(NSError **), NSError **error)
{
    for (unsigned attempt = 1;; ++attempt) {
        NSError *last = nil;
        if (call(&last))
            return YES;
        const BOOL dropped = [last.domain isEqualToString:NSCocoaErrorDomain] &&
                             (last.code == NSXPCConnectionInterrupted || last.code == NSXPCConnectionInvalid);
        if (!dropped || attempt == kIPCAttempts) {
            if (error)
                *error = last;
            return NO;
        }
        sleep(1);
    }
}

static NSArray *gathered(NSString *path, NSError **error)
{
    id<SwitchMIBundle> bundle = [[bundleClass alloc] initWithBundleURL:[NSURL fileURLWithPath:path isDirectory:YES]
                                                                 error:error];
    if (!bundle)
        return nil;
#if SWITCH_APPREGD_INSTALLATION_RECORDS
    NSArray *records = [gathererClass fetchInfoForBundle:bundle forPersonas:nil inContainer:nil withError:error];
#else
    NSArray *records = [gathererClass fetchInfoForBundle:bundle forPersona:nil inContainer:nil withError:error];
#endif
    if (!records.count && error && !*error)
        *error = failure(@"MobileInstallation gathered no record");
    return records.count ? records : nil;
}

BOOL switch_register_builtin_app(NSString *path, NSUUID *uuid, id<LSOperationSaveObserver> observer, NSError **error)
{
    NSArray *records = gathered(path, error);
    if (!records)
        return NO;
    id<SwitchLSApplicationWorkspace> workspace = [workspaceClass defaultWorkspace];
    id context = requestContext();
#if SWITCH_APPREGD_INSTALLATION_RECORDS
    NSArray *extensions = [records subarrayWithRange:NSMakeRange(1, records.count - 1)];
    return retrying(
        ^BOOL(NSError **last) {
            return [workspace registerContainerizedApplicationWithInstallationRecord:records.firstObject
                                                        extensionInstallationRecords:extensions
                                                                personaUniqueStrings:nil
                                                                       operationUUID:uuid
                                                                      requestContext:context
                                                                        saveObserver:observer
                                                                   registrationError:last] != nil;
        },
        error);
#else
    NSArray *infos = [recordClass bundleRecordArrayToInfoDictionaryArray:records];
    return retrying(
        ^BOOL(NSError **last) {
            return [workspace registerContainerizedApplicationWithInfoDictionaries:infos
                                                                     operationUUID:uuid
                                                                    requestContext:context
                                                                      saveObserver:observer
                                                                 registrationError:last] != nil;
        },
        error);
#endif
}

BOOL switch_unregister_builtin_app(NSString *path, NSString *identifier, NSUUID *uuid,
                                   id<LSOperationSaveObserver> observer, NSError **error)
{
    id<SwitchLSApplicationWorkspace> workspace = [workspaceClass defaultWorkspace];
    id context = requestContext();
#if SWITCH_APPREGD_INSTALLATION_RECORDS
    (void)identifier;
    NSURL *url = [NSURL fileURLWithPath:path isDirectory:YES];
    return retrying(
        ^BOOL(NSError **last) {
            return [workspace unregisterBuiltinApplicationAtURL:url
                                                  operationUUID:uuid
                                                 requestContext:context
                                                   saveObserver:observer
                                                          error:last];
        },
        error);
#else
    (void)path;
    static const unsigned kUninstallOperation = 3;
    return retrying(
        ^BOOL(NSError **last) {
            return [workspace unregisterContainerizedApplicationWithBundleIdentifier:identifier
                                                                       operationUUID:uuid
                                                             unregistrationOperation:kUninstallOperation
                                                                        precondition:nil
                                                                      requestContext:context
                                                                        saveObserver:observer
                                                                 unregistrationError:last];
        },
        error);
#endif
}
