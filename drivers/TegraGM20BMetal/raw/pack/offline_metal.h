// SPDX-License-Identifier: MIT
// The pack generator's libraries and functions with no m2v runtime (bridge/NativeMetalOffline.rs): made by the translator
// alone from a metallib's file or bytes, standing in for the frontend's wherever probe_states.h and kernel_shapes.h make a
// corpus line's state, so a state's key is made as before; and the direct compile of a descriptor of them.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <string>
#include "../runtime/RawDirect.h"

// The device calls the line parsers make of a library: newLibraryWithURL:error: and newLibraryWithData:error:
@interface RawOfflineDevice : NSObject
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
@end

namespace offline {

// The direct compile of descriptors of RawOfflineDevice's functions, NVK's exports looked up in the dylib at `nvk`
raw::DirectTranslator translator(const char *nvk);

// Whether the m2v runtime makes a compute state of `descriptor` (of RawOfflineDevice's functions): its own compile
// device compiles the kernel. False with `why`.
bool kernelMade(MTLComputePipelineDescriptor *descriptor, std::string &why);

// The answers the runtime's compute prepare gives `descriptor`'s state (of RawOfflineDevice's functions): its
// maxTotalThreadsPerThreadgroup, threadExecutionWidth and staticThreadgroupMemoryLength (raw_pack.h PackComputeFacts).
// False with `why`.
bool kernelFacts(MTLComputePipelineDescriptor *descriptor, uint32_t facts[3], std::string &why);

}  // namespace offline
