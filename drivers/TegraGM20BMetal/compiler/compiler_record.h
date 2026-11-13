// SPDX-License-Identifier: MIT
// A prepared request's record (compiler_request.h), made by raw::Direct from the functions of the libraries it names: the
// engine's over its compile context (compiler_engine.h). Who makes the libraries is the caller's: a record depends only on
// the functions and the Direct.
#pragma once
#import <Metal/Metal.h>
#include <functional>
#include <string>
#include "compiler_request.h"

namespace raw {
class Direct;
}

namespace switch_compiler {

// The library `l` names, nil with `why`
using Libraries = std::function<id<MTLLibrary>(const raw::RequestLibrary &l, std::string &why)>;

// `p`'s record or refusal as the store keeps it (raw_cache.h's entry bytes), keyed under `stack`; TegraGM20BCompilerABI.h's
// result
int32_t makeEntry(const Prepared &p, const Libraries &libraries, raw::Direct &direct, uint64_t stack,
                  std::string &entry, std::string &why);

}  // namespace switch_compiler
