// SPDX-License-Identifier: MIT
// A request as the compiler takes it (raw_compile_request.h): decoded, each system library's function list read from the
// system volume itself (a path that leaves /System, through a link or otherwise, is refused), and the state's CacheKey made
// from what was read, the way the runtime keys a state or a kernel. The key the client sent is only checked against it: a
// request whose fields or libraries give another key is refused, and nothing else the client computed is kept.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../raw/runtime/raw_compile_request.h"
#include "../raw/runtime/raw_metallib.h"

namespace switch_compiler {

// The function lists of the system libraries asked for, read once per path (the system volume does not change), from the
// system volumes at `root` ("" on the device)
class SystemLibraries {
public:
    explicit SystemLibraries(std::string root = "") : root_(std::move(root))
    {}

    bool air(const std::string &path, std::map<std::string, raw::AirHash> &out, std::string &why);
    // Where `path` is under `root`, once it is known to stay on the system volume: what the compiler opens
    static bool resolve(const std::string &root, const std::string &path, std::string &real, std::string &why);

private:
    const std::string root_;
    std::mutex lock_;
    std::map<std::string, std::map<std::string, raw::AirHash>> read_;
};

struct Prepared {
    raw::CompileRequest request;
    raw::CacheKey key;
    std::string packKey;  // the key Direct makes the record under (a kernel's compute key)
    raw::Digest name{};
};

bool prepare(const std::string &bytes, uint64_t stack, SystemLibraries &system, Prepared &out, std::string &why);

}  // namespace switch_compiler
