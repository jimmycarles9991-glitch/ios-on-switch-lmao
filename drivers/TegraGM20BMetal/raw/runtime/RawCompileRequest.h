// SPDX-License-Identifier: MIT
// A compile request's descriptor fields made into a descriptor again, and the pack key of a render request made the way the
// facade makes a state's (RawPipelineKey.h pipelineKey over the request's fields, RawMetalCubic.mm's variant name): the
// client and the shader compiler both go through these, so a field the request misses shows as a key that differs instead
// of a record made for another state.
#pragma once
#import <Metal/Metal.h>
#include <string>
#include "RawDirect.h"
#include "RawPipelineKey.h"
#include "raw_compile_request.h"

namespace raw {

// RawPipelineKey.h constantsText read back into values; nil for an item that names no size it can hold
MTLFunctionConstantValues *constantValuesOf(const std::string &text);

// The descriptor the compiler compiles a request from: its fields (RawPipelineKey.h fieldsOf read back), before its
// functions
void applyFields(const PipelineFields &f, MTLRenderPipelineDescriptor *d);

// The pack key `r`'s stages and fields make, with its variant's name, and whether the state is its libraries' own (no
// framework's default library: RawMetalStates.mm rmPipelineFor's _packable, negated)
std::string requestPackKey(const CompileRequest &r, bool &own);

StageInFields stageInFieldsOf(MTLStageInputOutputDescriptor *in);
// nil for no stage input
MTLStageInputOutputDescriptor *stageInOf(const StageInFields &f);

// What a variant's record is compiled with, by the client (RawComplete.mm) and the shader compiler alike
ProbeTextures probeTexturesOf(const RenderVariant &v);

}  // namespace raw
