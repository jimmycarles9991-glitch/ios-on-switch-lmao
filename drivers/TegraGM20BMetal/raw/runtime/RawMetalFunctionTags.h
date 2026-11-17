// SPDX-License-Identifier: MIT
// A function's reflection as Metal answers it from its metallib's public metadata (raw_function_tags.h), or from its AIR
// through the shader compiler: no twin made
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "raw_buffer_encoders.h"

@class RMFunction;

NSDictionary<NSString *, MTLFunctionConstant *> *rmFunctionConstants(RMFunction *f);

// vertexAttributes (`stageInput` false) or stageInputAttributes; nil where Metal answers nil
NSArray *rmFunctionAttributes(RMFunction *f, bool stageInput);

// The encoders at every buffer index `f` declares, the shader compiler's from its AIR; false with `why`
bool rmBufferEncoders(RMFunction *f, raw::BufferEncoders &out, std::string &why);
