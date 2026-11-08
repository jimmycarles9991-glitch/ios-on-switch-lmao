// SPDX-License-Identifier: MIT
// A Metal call the raw facade records as an invocation and replays on m2v at commit (RawMetalEncoder.mm forwardInvocation:): what its
// pointer arguments point to, and the copy of it the invocation keeps.
#pragma once
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <cstring>
#include <utility>
#include <vector>

namespace raw {

// The type a recorded argument's encoding names, past its qualifiers (const, in, out, ...)
inline const char *argumentType(NSMethodSignature *sig, NSUInteger i)
{
    const char *t = [sig getArgumentTypeAtIndex:i];
    while (*t == 'r' || *t == 'n' || *t == 'N' || *t == 'o' || *t == 'O' || *t == 'R' || *t == 'V')
        t++;
    return t;
}

inline bool countType(const char *t)
{
    return t[0] == 'Q' || t[0] == 'L' || t[0] == 'I';
}

// The elements a pointer argument of a Metal call points to: the length of the range after it (set...s:withRange:), else the
// count after it (setBytes:length:, setViewports:count:, useResources:count:), else the count just before it
// (setVertexAmplificationCount:viewMappings:); 0 where none does
inline NSUInteger pointedElements(NSInvocation *inv, NSUInteger i)
{
    NSMethodSignature *sig = inv.methodSignature;
    NSUInteger n = 0;
    for (NSUInteger k = i + 1; k < sig.numberOfArguments; k++) {
        const char *u = argumentType(sig, k);
        if (!strncmp(u, "{_NSRange=", 10)) {
            NSRange r = {};
            [inv getArgument:&r atIndex:k];
            return r.length;
        }
        if (countType(u)) {
            [inv getArgument:&n atIndex:k];
            return n;
        }
    }
    if (i > 2 && countType(argumentType(sig, i - 1)))
        [inv getArgument:&n atIndex:i - 1];
    return n;
}

// A call recorded to be replayed on m2v at commit: its pointer arguments' data (bytes, structs, offsets, object arrays) copied
// into storage the invocation keeps, and the objects of its arrays retained (retainArguments keeps objects, C strings and blocks
// alone; the rest still pointed into the caller's stack). False: a pointer argument whose extent no count names.
inline bool keepPointedArguments(NSInvocation *inv)
{
    NSMethodSignature *sig = inv.methodSignature;
    bool known = true;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = argumentType(sig, i);
        if (t[0] != '^' && t[0] != '*')
            continue;
        const void *from = nullptr;
        [inv getArgument:&from atIndex:i];
        if (!from || t[0] == '*')
            continue;
        const NSUInteger n = pointedElements(inv, i);
        if (!n) {
            known = false;
            continue;
        }
        NSUInteger size = 1, align = 1;
        if (t[1] != 'v')
            NSGetSizeAndAlignment(t + 1, &size, &align);
        NSMutableData *copy = [NSMutableData dataWithBytes:from length:n * size];
        id keep = copy;
        if (t[1] == '@') {
            NSMutableArray *objects = [NSMutableArray arrayWithObject:copy];
            id __unsafe_unretained const *array = (id __unsafe_unretained const *)from;
            for (NSUInteger k = 0; k < n; k++)
                if (array[k])
                    [objects addObject:array[k]];
            keep = objects;
        }
        objc_setAssociatedObject(inv, (__bridge const void *)copy, keep, OBJC_ASSOCIATION_RETAIN);
        void *to = copy.mutableBytes;
        [inv setArgument:&to atIndex:i];
    }
    return known;
}

// Each object a recorded call names: its object arguments and the elements of its object arrays
template <typename Visit> void forEachObject(NSInvocation *inv, Visit &&visit)
{
    NSMethodSignature *sig = inv.methodSignature;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = argumentType(sig, i);
        if (t[0] == '@') {
            __unsafe_unretained id arg = nil;
            [inv getArgument:&arg atIndex:i];
            visit(arg);
        } else if (t[0] == '^' && t[1] == '@') {
            id __unsafe_unretained *array = nullptr;
            [inv getArgument:&array atIndex:i];
            const NSUInteger n = array ? pointedElements(inv, i) : 0;
            for (NSUInteger k = 0; k < n; k++)
                visit(array[k]);
        }
    }
}

// The recorded call made on `target` with each object it names replaced by map(object), its own arguments put back after:
// the call keeps naming the objects it was recorded with (what a compute pass wrote is marked by them once it completes). An
// array's mapped copy stays with the invocation, in case the target keeps the pointer.
template <typename Map> void invokeMapped(NSInvocation *inv, id target, Map &&map)
{
    NSMethodSignature *sig = inv.methodSignature;
    NSMutableArray *originals = [NSMutableArray array];  // alive while the invocation names their replacements
    std::vector<std::pair<NSUInteger, void *>> arguments;
    for (NSUInteger i = 2; i < sig.numberOfArguments; i++) {
        const char *t = argumentType(sig, i);
        if (t[0] == '@') {
            __unsafe_unretained id arg = nil;
            [inv getArgument:&arg atIndex:i];
            id mapped = map(arg);
            if (mapped == arg)
                continue;
            if (arg)
                [originals addObject:arg];
            arguments.push_back({i, (__bridge void *)arg});
            __unsafe_unretained id m = mapped;
            [inv setArgument:&m atIndex:i];
        } else if (t[0] == '^' && t[1] == '@') {
            id __unsafe_unretained *array = nullptr;
            [inv getArgument:&array atIndex:i];
            const NSUInteger count = array ? pointedElements(inv, i) : 0;
            if (!count)
                continue;
            NSMutableData *copy = [NSMutableData dataWithLength:count * sizeof(id)];
            id __unsafe_unretained *out = (id __unsafe_unretained *)copy.mutableBytes;
            NSMutableArray *keep = [NSMutableArray array];
            for (NSUInteger k = 0; k < count; k++) {
                id m = map(array[k]);
                if (m)
                    [keep addObject:m];
                out[k] = m;
            }
            objc_setAssociatedObject(inv, (__bridge const void *)copy, @[ copy, keep ], OBJC_ASSOCIATION_RETAIN);
            arguments.push_back({i, (void *)array});
            id __unsafe_unretained *p = out;
            [inv setArgument:&p atIndex:i];
        }
    }
    [inv invokeWithTarget:target];
    for (auto [i, original] : arguments)
        [inv setArgument:&original atIndex:i];
}

}  // namespace raw
