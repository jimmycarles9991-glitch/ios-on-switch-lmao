// SPDX-License-Identifier: MIT
// A function's reflection as Metal answers it from its metallib's public metadata (function_tags.h), or from its AIR
// through the shader compiler: no twin made
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "buffer_encoders.h"

@class SwitchMetalFunction;

NSDictionary<NSString *, MTLFunctionConstant *> *gm20bFunctionConstants(SwitchMetalFunction *f);

// vertexAttributes (`stageInput` false) or stageInputAttributes; nil where Metal answers nil
NSArray *gm20bFunctionAttributes(SwitchMetalFunction *f, bool stageInput);

// The encoders at every buffer index `f` declares, the shader compiler's from its AIR; false with `why`
bool gm20bBufferEncoders(SwitchMetalFunction *f, gm20b::BufferEncoders &out, std::string &why);
