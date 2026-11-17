// SPDX-License-Identifier: MIT
// Libraries and functions with no m2v runtime (translate/NativeMetalOffline.rs): made by the translator alone from a
// metallib's file or bytes, for the pack generator (where probe_states.h and kernel_shapes.h make a corpus line's state,
// so a state's key is made as before) and the shader compiler daemon; and the direct compile of a descriptor of them.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <string>
#include <vector>
#include "RawDirect.h"
#include "../../translate/NativeMetalTranslate.h"

// The device calls the line parsers make of a library: newLibraryWithURL:error: and newLibraryWithData:error:
@interface RawOfflineDevice : NSObject
- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error;
- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error;
@end

namespace offline {

// The direct compile's translation of descriptors of RawOfflineDevice's functions
raw::DirectTranslator translator();

// Whether the m2v runtime makes a compute state of `descriptor` (of RawOfflineDevice's functions): its own compile
// device compiles the kernel. False with `why`.
bool kernelMade(MTLComputePipelineDescriptor *descriptor, std::string &why);

// The answers the runtime's compute prepare gives `descriptor`'s state (of RawOfflineDevice's functions): its
// maxTotalThreadsPerThreadgroup, threadExecutionWidth and staticThreadgroupMemoryLength (raw_pack.h PackComputeFacts).
// False with `why`.
bool kernelFacts(MTLComputePipelineDescriptor *descriptor, uint32_t facts[3], std::string &why);

// `d`'s descriptor as the bridge's offline exports take it, `linked` holding the handles it points into; false: a function
// of another device, or what the frontend does not represent
bool descriptorOf(MTLRenderPipelineDescriptor *d, NmbOfflineRenderPipelineDescriptor &p,
                  std::vector<NmbOfflineFunction *> &linked);
bool descriptorOf(MTLComputePipelineDescriptor *d, NmbOfflineComputePipelineDescriptor &p,
                  std::vector<NmbOfflineFunction *> &linked, std::vector<switch_linked_group_name> &groupNames);

// The reflection of `descriptor`'s state (of RawOfflineDevice's functions) as raw_reflection_blob.h lays it out
// (RawOfflineReflection.mm): what the pack and the system shader compiler's store hold of it. False with `why`.
bool renderReflection(MTLRenderPipelineDescriptor *descriptor, std::string &blob, std::string &why);
bool computeReflection(MTLComputePipelineDescriptor *descriptor, std::string &blob, std::string &why);

}  // namespace offline
