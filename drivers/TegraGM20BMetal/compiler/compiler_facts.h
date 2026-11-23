// SPDX-License-Identifier: MIT
// A compute state's facts (compile_request.h RequestKind::Facts) as the pack generator makes its PackComputeFacts
// (TegraGM20BMetalOffline.mm kernelFacts): the translator alone over the library's AIR slice, with no compile context,
// so a state's creation in a client that the pack lacks needs neither m2v nor a compile of its own.
#pragma once
#include <cstdint>
#include <string>
#include "compiler_request.h"

namespace switch_compiler {

// `p`'s facts or refusal as the store keeps it (compute_facts_entry.h), its system library read under `systemRoot`;
// TegraGM20BCompilerABI.h's result
int32_t makeFacts(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why);

// The attributes of `p`'s one function made with its constants (function_attributes.h), as a reflection entry
// (reflection_blob.h encodeReflectionEntry), from the translator alone as makeFacts' are
int32_t makeAttributes(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why);

// The buffer encoders of `p`'s one function (buffer_encoders.h) as a reflection entry, from the translator alone
int32_t makeBufferEncoders(const Prepared &p, const std::string &systemRoot, std::string &entry, std::string &why);

}  // namespace switch_compiler
