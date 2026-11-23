// SPDX-License-Identifier: MIT
// The GPU plugin's opens of a process's Metal device (TegraGM20BMetalPlugin.mm openDevice) and the device's
// (TegraGM20BMetal.mm), so a host test holds the rules. A cause every later open would meet again is structural: the process
// left out, the _MTLDevice ABI not the IPSW kind's, the pack another build's, unreadable or not staged, a GPU that is not
// GM20B. One the kernel answered for this moment is transient: no memory for the device's
// tables, no GPU VA, no channel. Either fails that open alone (nil, one error line naming the class); Metal's next ask
// opens afresh.
#pragma once
#include <cstdint>

namespace gm20b {

enum class OpenFailure : uint8_t {
    Structural,
    Transient,
};

constexpr const char *openFailureText(OpenFailure f)
{
    return f == OpenFailure::Transient ? "transient" : "structural";
}

}  // namespace gm20b
