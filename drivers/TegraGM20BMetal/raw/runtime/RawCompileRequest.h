// SPDX-License-Identifier: MIT
// A compile request's descriptor fields read off a descriptor and made into one again, and the pack key of a render request
// made the way the facade makes a state's (RawPipelineKey.h pipelineKey over the descriptor, RawMetalCubic.mm's variant
// suffixes): the client and the shader compiler both go through these, so a field the request misses shows as a key that
// differs instead of a record made for another state.
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "raw_compile_request.h"

namespace raw {

// RawPipelineKey.h constantsText read back into values; nil for an item that names no size it can hold
MTLFunctionConstantValues *constantValuesOf(const std::string &text);

PipelineFields fieldsOf(MTLRenderPipelineDescriptor *d);
void applyFields(const PipelineFields &f, MTLRenderPipelineDescriptor *d);

// The pack key `r`'s stages and fields make, with its variant's suffix, and whether the state is its libraries' own (no
// framework's default library: RawMetalStates.mm rmPipelineFor's _packable, negated)
std::string requestPackKey(const CompileRequest &r, bool &own);

}  // namespace raw
