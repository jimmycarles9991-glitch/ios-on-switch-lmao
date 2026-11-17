// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "../CapabilityPolicy.h"
#import "../EntitlementDER.h"
#include <assert.h>
#include <CommonCrypto/CommonDigest.h>
#include <dlfcn.h>
#include <libkern/OSByteOrder.h>
#include <mach-o/loader.h>
#include <sys/stat.h>

void switch_policy_capability_tests(NSURL *root)
{
    NSDictionary *declared = @{
        @"application-identifier" : @"TEAM.org.example.app",
        @"com.apple.developer.team-identifier" : @"TEAM",
        @"keychain-access-groups" : @[@"TEAM.shared", @"TEAM.com.apple.token", @"*", @"com.apple.account", @"apple"],
        @"com.apple.security.application-groups" : @[@"group.org.example.shared", @"systemgroup.com.apple"],
        @"com.apple.developer.associated-domains" : @[@"applinks:example.org"],
        @"com.apple.developer.parent-application-identifiers" : @[@"OTHER.unrelated.parent", @"TEAM.com.apple.parent", @"*"],
        @"com.apple.developer.associated-application-identifier" : @"OTHER.unrelated.application",
        @"aps-environment" : @"production",
        @"org.example.capability" : @YES,
        @"com.apple.private.security.no-sandbox" : @YES,
        @"com.apple.developer.system-extension.install" : @YES,
        @"com.apple.developer.driverkit" : @YES,
        @"com.apple.developer.endpoint-security.client" : @YES,
        @"com.apple.developer.kernel.extended-virtual-addressing" : @YES,
        @"platform-application" : @YES,
        @"get-task-allow" : @YES,
        @"task_for_pid-allow" : @YES,
        @"seatbelt-profiles" : @[@"unrestricted"],
        @"keychain-cloud-circle" : @YES,
        @"keychain-migrate-allow" : @YES,
        @"unknown-privilege" : @YES,
    };
    NSDictionary *effective = switch_filter_capabilities(declared, @"org.example.fallback", nil);
    assert([effective[@"application-identifier"] isEqual:@"TEAM.org.example.app"]);
    assert([effective[@"keychain-access-groups"] isEqual:@[@"TEAM.shared"]]);
    assert([effective[@"com.apple.security.application-groups"] isEqual:@[@"group.org.example.shared"]]);
    assert([effective[@"org.example.capability"] boolValue]);
    assert([effective[@"com.apple.developer.parent-application-identifiers"] isEqual:@[@"OTHER.unrelated.parent"]]);
    assert([effective[@"com.apple.developer.associated-application-identifier"] isEqual:@"OTHER.unrelated.application"]);
    assert(effective.count == 9);
    NSString *longIdentity = [@"a" stringByPaddingToLength:256 withString:@"a" startingAtIndex:0];
    assert([switch_filter_capabilities(@{@"application-identifier" : longIdentity}, @"org.example.base", nil)
        [@"application-identifier"] isEqual:@"org.example.base"]);
    assert(([switch_filter_capabilities(@"malformed", @"org.example.app", nil)
        isEqual:@{@"application-identifier" : @"org.example.app", @"keychain-access-groups" : @[@"org.example.app"]}]));
    NSString *huge = [@"x" stringByPaddingToLength:65536 withString:@"x" startingAtIndex:0];
    assert(([switch_filter_capabilities(@{@"org.example.huge" : huge}, @"org.example.app", nil)
        isEqual:@{@"application-identifier" : @"org.example.app", @"keychain-access-groups" : @[@"org.example.app"]}]));
    assert([switch_filter_capabilities(@{}, @"com.apple.system", nil)[@"application-identifier"] hasPrefix:@"Switch."]);
    NSURL *app = [root URLByAppendingPathComponent:@"Capabilities.app"];
    assert([[NSFileManager defaultManager] createDirectoryAtURL:app withIntermediateDirectories:YES attributes:nil error:nil]);
    NSURL *executable = [app URLByAppendingPathComponent:@"Main"];
    NSURL *helper = [app URLByAppendingPathComponent:@"Helper"];
    uint32_t magic = 0xfeedfacf;
    NSData *image = [NSData dataWithBytes:&magic length:sizeof(magic)];
    assert([image writeToURL:executable atomically:YES] && [image writeToURL:helper atomically:YES]);
    assert(switch_declared_capabilities(executable).count == 0);
    assert(switch_declared_capabilities([app URLByAppendingPathComponent:@"Missing"]).count == 0);
    NSError *error = nil;
    assert(switch_publish_capabilities(app, app, @{executable.URLByResolvingSymlinksInPath.path : effective},
        @{app.URLByResolvingSymlinksInPath.path : effective}, &error));
    NSData *mainData = [NSData dataWithContentsOfFile:[executable.path stringByAppendingString:@".SwitchEntitlements"]];
    NSData *helperData = [NSData dataWithContentsOfFile:[helper.path stringByAppendingString:@".SwitchEntitlements"]];
    NSDictionary *mainInfo = switch_deserialize_entitlements(mainData);
    NSDictionary *helperInfo = switch_deserialize_entitlements(helperData);
    assert([mainInfo isEqual:effective] && [helperInfo isEqual:effective]);
    assert([mainData isEqual:switch_serialize_entitlements(effective)] && [helperData isEqual:mainData]);
    struct stat status;
    assert(!lstat([executable.path stringByAppendingString:@".SwitchEntitlements"].fileSystemRepresentation, &status));
    assert(S_ISREG(status.st_mode) && (status.st_mode & 0777) == 0600 && status.st_nlink == 1);
}

void switch_policy_source_tests(NSURL *root)
{
    NSDictionary *declarations = @{@"application-identifier" : @"TEAM.org.example.unverified",
        @"com.apple.developer.team-identifier" : @"TEAM",
        @"keychain-access-groups" : @[@"TEAM.shared"], @"platform-application" : @YES,
        @"com.apple.developer.on-demand-install-capable" : @YES,
        @"com.apple.developer.parent-application-identifiers" : @[@"OTHER.unrelated.parent"]};
    NSData *xml = [NSPropertyListSerialization dataWithPropertyList:declarations format:NSPropertyListXMLFormat_v1_0
                                                          options:0 error:nil];
    const char identifier[] = "org.example.unverified";
    uint32_t directoryLength = 44 + sizeof(identifier) + 192;
    uint32_t totalLength = 28 + directoryLength + 8 + (uint32_t)xml.length;
    NSMutableData *image = [NSMutableData dataWithLength:4096 + totalLength];
    auto header = (mach_header_64 *)image.mutableBytes;
    *header = {MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64_ALL, MH_EXECUTE, 2,
        sizeof(segment_command_64) + sizeof(linkedit_data_command), 0, 0};
    auto segment = (segment_command_64 *)(header + 1);
    segment->cmd = LC_SEGMENT_64;
    segment->cmdsize = sizeof(*segment);
    memcpy(segment->segname, "__TEXT", 6);
    segment->vmaddr = UINT64_C(0x100000000);
    segment->vmsize = segment->filesize = 4096;
    segment->maxprot = segment->initprot = 5;
    auto command = (linkedit_data_command *)(segment + 1);
    *command = {LC_CODE_SIGNATURE, sizeof(*command), 4096, totalLength};
    auto signature = (uint32_t *)((uint8_t *)image.mutableBytes + 4096);
    const uint32_t index[] = {0xfade0cc0, totalLength, 2, 0, 28, 5, 28 + directoryLength};
    for (size_t i = 0; i < sizeof(index) / sizeof(index[0]); ++i)
        signature[i] = OSSwapHostToBigInt32(index[i]);
    auto directory = (uint32_t *)((uint8_t *)signature + 28);
    const uint32_t fields[] = {0xfade0c02, directoryLength, 0x20001, 2, 204 + sizeof(identifier), 44, 5, 1, 4096};
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i)
        directory[i] = OSSwapHostToBigInt32(fields[i]);
    auto bytes = (uint8_t *)directory;
    bytes[36] = 32;
    bytes[37] = 2;
    bytes[39] = 12;
    memcpy(bytes + 44, identifier, sizeof(identifier));
    uint8_t *entitlements = bytes + directoryLength;
    const uint32_t entitlementHeader[] = {OSSwapHostToBigInt32(0xfade7171), OSSwapHostToBigInt32(8 + (uint32_t)xml.length)};
    memcpy(entitlements, entitlementHeader, sizeof(entitlementHeader));
    memcpy(entitlements + 8, xml.bytes, xml.length);
    CC_SHA256(entitlements, 8 + (CC_LONG)xml.length, bytes + 44 + sizeof(identifier));
    NSURL *url = [root URLByAppendingPathComponent:@"UnverifiedSource"];
    assert([image writeToURL:url atomically:YES]);
    NSDictionary *read = switch_declared_capabilities(url);
    for (NSString *key in declarations)
        assert([read[key] isEqual:declarations[key]]);
    void *security = dlopen("/System/Library/Frameworks/Security.framework/Security", RTLD_LAZY);
    auto create = (int32_t (*)(CFURLRef, uint32_t, CFTypeRef *))dlsym(security, "SecStaticCodeCreateWithPath");
    auto validate = (int32_t (*)(CFTypeRef, uint32_t, CFTypeRef))dlsym(security, "SecStaticCodeCheckValidity");
    CFTypeRef code = nullptr;
    assert(create && validate && !create((__bridge CFURLRef)url, 0, &code) && code);
    assert(validate(code, 0, nullptr) != 0);
    CFRelease(code);
    NSDictionary *effective = switch_filter_capabilities(read, @"org.example.app", nil);
    assert([effective[@"keychain-access-groups"] isEqual:@[@"TEAM.shared"]]);
    assert(effective[@"platform-application"] == nil);
}
