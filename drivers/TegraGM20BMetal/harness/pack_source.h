// SPDX-License-Identifier: MIT
// A pack Metal file as one source string for newLibraryWithSource:, which resolves no #include: each `#include "name"`
// of a file in the pack's directory replaced in place by that file's own source, its `#pragma once` dropped
#pragma once
#import <Foundation/Foundation.h>

inline NSString *packSource(NSString *dir, NSString *file)
{
    NSString *text = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:file]
                                               encoding:NSUTF8StringEncoding
                                                  error:nil];
    if (!text)
        return nil;
    NSMutableString *out = [NSMutableString string];
    for (NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSString *bare = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if ([bare isEqualToString:@"#pragma once"])
            continue;
        if ([bare hasPrefix:@"#include \""] && [bare hasSuffix:@"\""] && bare.length > 11) {
            NSString *inner = packSource(dir, [bare substringWithRange:NSMakeRange(10, bare.length - 11)]);
            if (!inner)
                return nil;
            [out appendString:inner];
        } else {
            [out appendString:line];
        }
        [out appendString:@"\n"];
    }
    return out;
}
