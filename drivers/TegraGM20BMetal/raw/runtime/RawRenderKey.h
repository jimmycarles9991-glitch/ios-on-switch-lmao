// SPDX-License-Identifier: MIT
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "raw_render_key.h"

// A function that holds its library's AIR slice and its entry there itself (the pack generator's offline functions:
// pack/offline_metal.h); the frontend answers for its own
@protocol RawLibraryFunction
- (const void *)libraryBytes:(size_t *)length;
- (NSString *)libraryFunctionName;
@end

namespace raw {

// The AIR slice `function`'s library holds and the function's entry there; false for a function that names neither
bool functionLibrary(id<MTLFunction> function, const void *&bytes, size_t &length, std::string &entry);

// A function's metallib bytes (the AIR slice its library was made from) and the library function it was made from, as
// whoever made the descriptor's functions keeps them; false: none
using FunctionSource = bool (*)(id<MTLFunction> function, const void **bytes, size_t *length, NSString **name);

// `source`: how the descriptor's functions name their AIR; null: functionLibrary's (the pack generator's offline functions,
// or m2v's)
bool renderCacheKey(MTLRenderPipelineDescriptor *descriptor, const std::string &key, bool own, uint64_t stack,
                    CacheKey &out, FunctionSource source = nullptr);

}  // namespace raw
