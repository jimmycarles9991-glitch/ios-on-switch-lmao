// SPDX-License-Identifier: MIT
// The compiler's own sandbox, entered once its store, request log, GPU connection and m2v device are open: it parses
// metallibs any GPU client sends, so from then on it reads only the system volumes and its store root, writes only that
// root, opens only the GPU's user client, looks up only the log daemons, and execs, forks and networks nothing.
#pragma once
#include <string>

namespace switch_compiler {

// False with `why`: the kernel did not take the profile (the compiler then runs without it and says so)
bool enterSandbox(const std::string &storeRoot, std::string &why);

}  // namespace switch_compiler
