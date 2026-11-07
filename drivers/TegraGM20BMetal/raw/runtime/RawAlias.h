// SPDX-License-Identifier: MIT
// m2v's views of the raw path's memory (RawMetal.mm's twins): made over the memory itself in the GPU VM the two drivers share
// (raw_kernel.h), with no import, bind or copy, at their first use; and the frontend's IOSurface imports, which become the raw
// path's own, so the process imports each surface once whichever driver needs it first.
#pragma once
#import <Metal/Metal.h>
#include "raw_gpu.h"

// The frontend's IOSurface imports are dev's (raw::Device::aliasSurface) from now on.
void rawAliasSurfaces(raw::Device *dev);
// m2v's buffer over `length` bytes at `bytes` of `a` (raw::Device::alias); the caller's hold of a.lease is dropped. Nil: m2v
// refused.
id<MTLBuffer> rawAliasBuffer(raw::Alias &a, void *bytes, NSUInteger length, MTLResourceOptions options);
