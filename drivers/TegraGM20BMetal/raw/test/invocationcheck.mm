// SPDX-License-Identifier: MIT
// raw/runtime/raw_invocation.h: a call recorded as an invocation keeps what its pointer arguments pointed to when it was made.
// Each call is recorded over arrays on the stack, which are then overwritten, as the caller's frame would be reused before the
// commit replays the call; the replay must see the values of the call, and the objects of an array must still be alive.
// usage: invocationcheck
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdio>
#include "../runtime/raw_invocation.h"

@interface Kept : NSObject
@property NSUInteger tag;
@end

@implementation Kept
@end

@interface Recorder : NSObject {
@public
    uint8_t bytes[64];
    NSUInteger length;
    MTLViewport viewport;
    NSUInteger count, tags[4], offsets[4];
    NSRange range;
    uint32_t mapping;
}
@end

@implementation Recorder

- (void)setBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)index
{
    (void)index;
    length = l;
    memcpy(bytes, b, l);
}

- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n
{
    count = n;
    viewport = v[0];
}

- (void)setVertexBuffers:(const id __unsafe_unretained[])b offsets:(const NSUInteger[])o withRange:(NSRange)r
{
    range = r;
    for (NSUInteger k = 0; k < r.length; k++) {
        tags[k] = ((Kept *)b[k]).tag;
        offsets[k] = o[k];
    }
}

- (void)useResources:(const id __unsafe_unretained[])r count:(NSUInteger)n usage:(NSUInteger)usage
{
    (void)usage;
    count = n;
    for (NSUInteger k = 0; k < n; k++)
        tags[k] = ((Kept *)r[k]).tag;
}

- (void)setVertexAmplificationCount:(NSUInteger)n viewMappings:(const MTLVertexAmplificationViewMapping *)m
{
    count = n;
    mapping = m[n - 1].renderTargetArrayIndexOffset;
}

@end

static int failed;

static void check(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    failed += !ok;
}

static NSInvocation *call(Recorder *r, SEL selector)
{
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[r methodSignatureForSelector:selector]];
    inv.selector = selector;
    return inv;
}

static NSInvocation *recordBytes(Recorder *r)
{
    uint8_t data[24];
    for (uint8_t k = 0; k < sizeof data; k++)
        data[k] = uint8_t(k * 3 + 1);
    NSInvocation *inv = call(r, @selector(setBytes:length:atIndex:));
    const void *p = data;
    NSUInteger length = sizeof data, index = 2;
    [inv setArgument:&p atIndex:2];
    [inv setArgument:&length atIndex:3];
    [inv setArgument:&index atIndex:4];
    [inv retainArguments];
    check(raw::keepPointedArguments(inv), "setBytes:length:atIndex: has a known extent");
    memset(data, 0xee, sizeof data);
    return inv;
}

static NSInvocation *recordViewports(Recorder *r)
{
    MTLViewport v[1] = {{4, 8, 48, 24, 0, 1}};
    NSInvocation *inv = call(r, @selector(setViewports:count:));
    const MTLViewport *p = v;
    NSUInteger n = 1;
    [inv setArgument:&p atIndex:2];
    [inv setArgument:&n atIndex:3];
    [inv retainArguments];
    check(raw::keepPointedArguments(inv), "setViewports:count: has a known extent");
    memset(v, 0x7f, sizeof v);
    return inv;
}

static NSInvocation *recordBuffers(Recorder *r)
{
    Kept *a = [Kept new], *b = [Kept new];
    a.tag = 11;
    b.tag = 22;
    id __unsafe_unretained objects[2] = {a, b};
    NSUInteger offsets[2] = {256, 512};
    NSInvocation *inv = call(r, @selector(setVertexBuffers:offsets:withRange:));
    const void *po = objects, *pf = offsets;
    NSRange range = NSMakeRange(5, 2);
    [inv setArgument:&po atIndex:2];
    [inv setArgument:&pf atIndex:3];
    [inv setArgument:&range atIndex:4];
    [inv retainArguments];
    check(raw::keepPointedArguments(inv), "setVertexBuffers:offsets:withRange: has a known extent");
    memset(objects, 0, sizeof objects);
    memset(offsets, 0xab, sizeof offsets);
    return inv;
}

static NSInvocation *recordResources(Recorder *r)
{
    Kept *a = [Kept new];
    a.tag = 33;
    id __unsafe_unretained objects[1] = {a};
    NSInvocation *inv = call(r, @selector(useResources:count:usage:));
    const void *po = objects;
    NSUInteger n = 1, usage = 1;
    [inv setArgument:&po atIndex:2];
    [inv setArgument:&n atIndex:3];
    [inv setArgument:&usage atIndex:4];
    [inv retainArguments];
    check(raw::keepPointedArguments(inv), "useResources:count:usage: has a known extent");
    memset(objects, 0, sizeof objects);
    return inv;
}

static NSInvocation *recordMappings(Recorder *r)
{
    MTLVertexAmplificationViewMapping m[2] = {{0, 1}, {0, 7}};
    NSInvocation *inv = call(r, @selector(setVertexAmplificationCount:viewMappings:));
    NSUInteger n = 2;
    const void *p = m;
    [inv setArgument:&n atIndex:2];
    [inv setArgument:&p atIndex:3];
    [inv retainArguments];
    check(raw::keepPointedArguments(inv), "setVertexAmplificationCount:viewMappings: has a known extent");
    memset(m, 0, sizeof m);
    return inv;
}

int main()
{
    @autoreleasepool {
        Recorder *r = [Recorder new];
        NSInvocation *bytes = recordBytes(r), *viewports = recordViewports(r), *mappings = recordMappings(r);
        NSInvocation *buffers = nil, *resources = nil;
        @autoreleasepool {
            buffers = recordBuffers(r);
            resources = recordResources(r);
        }
        [bytes invokeWithTarget:r];
        bool same = r->length == 24;
        for (uint8_t k = 0; k < 24; k++)
            same &= r->bytes[k] == uint8_t(k * 3 + 1);
        check(same, "setBytes: replays the call's 24 bytes");
        [viewports invokeWithTarget:r];
        check(r->count == 1 && r->viewport.originX == 4 && r->viewport.width == 48 && r->viewport.zfar == 1,
              "setViewports: replays the call's viewport");
        [buffers invokeWithTarget:r];
        check(r->range.location == 5 && r->range.length == 2 && r->tags[0] == 11 && r->tags[1] == 22 &&
                  r->offsets[0] == 256 && r->offsets[1] == 512,
              "setVertexBuffers: replays its two buffers, alive, and their offsets");
        [resources invokeWithTarget:r];
        check(r->count == 1 && r->tags[0] == 33, "useResources: replays its resource, alive");
        [mappings invokeWithTarget:r];
        check(r->count == 2 && r->mapping == 7, "setVertexAmplificationCount: replays its mappings");
        printf("invocationcheck: %d failed\n", failed);
        return failed ? 1 : 0;
    }
}
