// SPDX-License-Identifier: MIT
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "../NativeBridgeMetalFrontend.h"

static void missingNameSelector(const char *type, const char *selector)
{
    fprintf(stderr, "MISSING %s %s\n", type, selector);
}

int main(int argc, char **argv)
{
    if (argc != 5)
        return 2;
    @autoreleasepool {
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> device = native_bridge_metal_open(argv[1], argv[2], 0, missingNameSelector);
        NSError *error = nil;
        id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(argv[3])] error:&error];
        NSData *data = [NSData dataWithContentsOfFile:@(argv[4])];
        NSArray<NSString *> *expected =
            data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:&error] : nil;
        if (!library || ![expected isKindOfClass:NSArray.class] || !expected.count) {
            fprintf(stderr, "cannot load name fixture: %s\n", error.localizedDescription.UTF8String);
            return 1;
        }
        unsigned failures = 0, checks = 0;
        const auto check = [&](bool good, const char *label) {
            printf("%s %s\n", good ? "PASS" : "FAIL", label);
            ++checks;
            failures += !good;
        };
        NSArray<NSString *> *names = library.functionNames;
        check([NSSet setWithArray:names].count == expected.count &&
                  [[NSSet setWithArray:names] isEqual:[NSSet setWithArray:expected]],
              "complete function names");
        for (NSString *name in expected) {
            id<MTLFunction> function = [library newFunctionWithName:name];
            check(function && [function.name isEqualToString:name], "lookup preserves exact function name");
            MTLFunctionConstantValues *values = [MTLFunctionConstantValues new];
            function = [library newFunctionWithName:name constantValues:values error:&error];
            check(function && [function.name isEqualToString:name], "specialization preserves exact function name");
        }
        check(![library newFunctionWithName:@"not_present_in_name_fixture"], "absent function remains absent");
        printf("%u checks, %u failures\n", checks, failures);
        return failures ? 1 : 0;
    }
}
