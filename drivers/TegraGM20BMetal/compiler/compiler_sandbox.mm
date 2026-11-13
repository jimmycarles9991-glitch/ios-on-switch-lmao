// SPDX-License-Identifier: MIT
// See compiler_sandbox.h. libsandbox compiles the profile in this process and the kernel applies it to it alone; a
// process may only ever narrow its own sandbox.
#import "compiler_sandbox.h"

#include <stdlib.h>

extern "C" int sandbox_init_with_parameters(const char *profile, uint64_t flags, const char *const parameters[],
                                            char **errorbuf);
extern "C" void sandbox_free_error(char *errorbuf);

namespace switch_compiler {
namespace {

constexpr const char kProfile[] = R"((version 1)
(deny default)
(allow file-read*
    (subpath "/System")
    (subpath "/usr/lib")
    (subpath "/usr/share")
    (subpath "/private/preboot/Cryptexes")
    (subpath "/private/var/Switch/overlay")
    (literal "/dev/null")
    (literal "/dev/urandom")
    (literal "/private/var/db/timezone/localtime"))
(allow file-read* file-write* (subpath (param "STORE_ROOT")))
(allow file-read-metadata (literal "/") (literal "/private") (literal "/private/var"))
(allow iokit* (iokit-registry-entry-class "AGXDeviceUserClient"))
(allow iokit-open-service (iokit-registry-entry-class "AGXAccelerator"))
(allow iokit-get-properties)
(allow mach-lookup
    (global-name "com.apple.logd")
    (global-name "com.apple.logd.events")
    (global-name "com.apple.diagnosticd"))
(allow sysctl-read)
(allow process-info* (target self))
(allow signal (target self))
)";

}  // namespace

bool enterSandbox(const std::string &storeRoot, std::string &why)
{
    const char *const parameters[] = {"STORE_ROOT", storeRoot.c_str(), nullptr};
    char *error = nullptr;
    if (!sandbox_init_with_parameters(kProfile, 0, parameters, &error))
        return true;
    why = error ? error : "sandbox_init_with_parameters failed";
    if (error)
        sandbox_free_error(error);
    return false;
}

}  // namespace switch_compiler
