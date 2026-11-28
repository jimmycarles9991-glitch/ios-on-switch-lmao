// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "../BundleDiscovery.h"

#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static NSUInteger switch_discovery_calls;
static NSMutableArray *switch_discovery_directories;

@interface SwitchDiscoveryFixtureBundle : NSObject
@property NSURL *bundleURL;
@property (weak) SwitchDiscoveryFixtureBundle *parentBundle;
@property BOOL nativeConstruction;
+ (NSArray *)bundlesInParentBundle:(SwitchDiscoveryFixtureBundle *)parent subDirectory:(NSString *)directory
                matchingPredicate:(BOOL (^)(NSURL *))predicate error:(NSError **)error;
@end

@implementation SwitchDiscoveryFixtureBundle
+ (NSArray *)bundlesInParentBundle:(SwitchDiscoveryFixtureBundle *)parent subDirectory:(NSString *)directory
                matchingPredicate:(BOOL (^)(NSURL *))predicate error:(NSError **)error
{
    ++switch_discovery_calls;
    [switch_discovery_directories addObject:directory];
    NSURL *root = [parent.bundleURL URLByAppendingPathComponent:directory isDirectory:YES];
    NSArray *entries = [[NSFileManager defaultManager] contentsOfDirectoryAtURL:root includingPropertiesForKeys:nil
                                                                     options:0 error:error];
    if (!entries)
        return nil;
    NSMutableArray *result = [NSMutableArray array];
    for (NSURL *url in entries) {
        if (!predicate(url))
            continue;
        SwitchDiscoveryFixtureBundle *bundle = [self new];
        bundle.bundleURL = url;
        bundle.parentBundle = parent;
        bundle.nativeConstruction = YES;
        [result addObject:bundle];
    }
    return result;
}
@end

@interface SwitchDiscoveryFixtureModern : SwitchDiscoveryFixtureBundle
@end
@implementation SwitchDiscoveryFixtureModern
@end

static NSURL *switch_discovery_fixture(NSURL *root, NSString *relative, NSDictionary *metadata)
{
    NSURL *url = [root URLByAppendingPathComponent:relative isDirectory:YES];
    assert([[NSFileManager defaultManager] createDirectoryAtURL:url withIntermediateDirectories:YES attributes:nil error:nil]);
    if (metadata) {
        NSData *data = [NSPropertyListSerialization dataWithPropertyList:metadata format:NSPropertyListXMLFormat_v1_0
                                                               options:0 error:nil];
        assert([data writeToURL:[url URLByAppendingPathComponent:@"Info.plist"] atomically:YES]);
    }
    return url;
}

int main(void)
{
    @autoreleasepool {
        char temporary[] = "/private/tmp/switch-discovery.XXXXXX";
        assert(mkdtemp(temporary));
        NSURL *root = [NSURL fileURLWithPath:@(temporary) isDirectory:YES];
        NSURL *app = switch_discovery_fixture(root, @"Main.app", nil);
        NSDictionary *legacy = @{@"NSExtension" : @{@"NSExtensionPointIdentifier" : @"org.example.point"}};
        NSDictionary *modern = @{@"EXAppExtensionAttributes" : @{@"EXExtensionPointIdentifier" : @"org.example.modern"}};
        NSURL *normal = switch_discovery_fixture(app, @"PlugIns/Normal.appex", legacy);
        switch_discovery_fixture(app, @"Arbitrary/Deep/NoSuffix", legacy);
        switch_discovery_fixture(app, @"Frameworks/Library.framework/Nested.app/Outer.appex/Inside", legacy);
        switch_discovery_fixture(app, @"XPCServices/Service.xpc/Modern.bundle", modern);
        switch_discovery_fixture(app, @"Extensions/Both", @{@"NSExtension" : @{}, @"EXAppExtensionAttributes" : @{}});
        switch_discovery_fixture(app, @"Frameworks/Ordinary.framework", @{@"CFBundlePackageType" : @"FMWK"});
        NSURL *outside = switch_discovery_fixture(root, @"Outside.appex", legacy);
        assert(!symlink(outside.fileSystemRepresentation,
            [app URLByAppendingPathComponent:@"Linked"].fileSystemRepresentation));
        NSURL *linkedInfo = switch_discovery_fixture(app, @"LinkedInfo", nil);
        assert(!symlink([outside URLByAppendingPathComponent:@"Info.plist"].fileSystemRepresentation,
            [linkedInfo URLByAppendingPathComponent:@"Info.plist"].fileSystemRepresentation));
        assert([@"asset" writeToURL:[app URLByAppendingPathComponent:@"Icon.png"]
                         atomically:YES encoding:NSUTF8StringEncoding error:nil]);
        SwitchDiscoveryFixtureBundle *parent = [SwitchDiscoveryFixtureBundle new];
        parent.bundleURL = app;
        SwitchDiscoveryFixtureBundle *known = [SwitchDiscoveryFixtureBundle new];
        known.bundleURL = normal;
        known.parentBundle = parent;
        switch_discovery_directories = [NSMutableArray array];
        NSError *error = nil;
        NSArray *plugins = switch_discover_extensions(parent, SwitchDiscoveryFixtureBundle.class, NO, @[known], &error);
        assert(plugins.count == 3 && !error);
        assert([plugins containsObject:known]);
        NSMutableSet *paths = [NSMutableSet set];
        for (SwitchDiscoveryFixtureBundle *bundle in plugins) {
            assert(bundle.parentBundle == parent);
            assert(bundle == known || bundle.nativeConstruction);
            [paths addObject:bundle.bundleURL.path];
        }
        assert(paths.count == plugins.count);
        assert([switch_discovery_directories containsObject:@"Arbitrary/Deep"]);
        assert([switch_discovery_directories containsObject:@"Frameworks/Library.framework/Nested.app/Outer.appex"]);
        NSUInteger calls = switch_discovery_calls;
        assert(switch_discover_extensions(parent, SwitchDiscoveryFixtureBundle.class, NO, @[], &error) == plugins);
        assert(calls == switch_discovery_calls);
        // ExtensionKit reuses the single cached directory walk from the PluginKit discovery call.
        switch_discovery_fixture(app, @"Extensions/AddedAfterScan", modern);
        NSArray *extensions = switch_discover_extensions(parent, SwitchDiscoveryFixtureModern.class, YES, @[], &error);
        assert(extensions.count == 2 && !error);
        for (SwitchDiscoveryFixtureBundle *bundle in extensions)
            assert([bundle isKindOfClass:SwitchDiscoveryFixtureModern.class] && bundle.nativeConstruction);
        assert([switch_discover_extensions(plugins[0], SwitchDiscoveryFixtureBundle.class, NO, @[], &error) count] == 0);
        SwitchDiscoveryFixtureBundle *missing = [SwitchDiscoveryFixtureBundle new];
        missing.bundleURL = [root URLByAppendingPathComponent:@"Missing"];
        assert(!switch_discover_extensions(missing, SwitchDiscoveryFixtureBundle.class, NO, @[], &error) && error);
        NSURL *deepRoot = switch_discovery_fixture(root, @"Deep", nil);
        NSURL *deep = deepRoot;
        for (NSUInteger depth = 0; depth < 130; ++depth)
            deep = switch_discovery_fixture(deep, @"d", nil);
        SwitchDiscoveryFixtureBundle *bounded = [SwitchDiscoveryFixtureBundle new];
        bounded.bundleURL = deepRoot;
        assert(!switch_discover_extensions(bounded, SwitchDiscoveryFixtureBundle.class, NO, @[], &error));
        assert(error.code == E2BIG);
        assert([[NSFileManager defaultManager] removeItemAtURL:root error:nil]);
    }
    return 0;
}
