// SPDX-License-Identifier: MIT
// A prepared request's record (compiler_request.h), made by gm20b::Direct from the functions of the libraries it names: the
// engine's from the translator alone (compiler_engine.h). Who makes the libraries is the caller's: a record depends only
// on the functions and the Direct.
#pragma once
#import <Metal/Metal.h>
#include <functional>
#include <string>
#include <vector>
#include "compiler_request.h"

namespace gm20b {
class Direct;
}

namespace switch_compiler {

// The library `l` names, nil with `why`
using Libraries = std::function<id<MTLLibrary>(const gm20b::RequestLibrary &l, std::string &why)>;

// The function of each of `r`'s stages in stage order, of `libraries`, specialized as the stage names; false with `why`
bool functions(const gm20b::CompileRequest &r, const Libraries &libraries, std::vector<id<MTLFunction>> &out,
               std::string &why);

// `p`'s record or refusal as the store keeps it (gm20b_cache.h's entry bytes), keyed under `stack`; TegraGM20BCompilerABI.h's
// result
int32_t makeEntry(const Prepared &p, const Libraries &libraries, gm20b::Direct &direct, uint64_t stack,
                  std::string &entry, std::string &why);
// A full form's made record `entry` (of `p.content`) headed by the content it adds to (Prepared::primaryContent), which
// the state's or twin's own lookup reads; empty where `entry` holds no record
std::string asPrimary(const Prepared &p, const std::string &entry);
// A Reflection request's entry (reflection_blob.h encodeReflectionEntry): the state's reflection as the pack generator
// makes it (TegraGM20BMetalOffline.h), of `libraries`' offline functions, with no compile context
int32_t makeReflectionEntry(const Prepared &p, const Libraries &libraries, uint64_t stack, std::string &entry,
                            std::string &why);

}  // namespace switch_compiler
