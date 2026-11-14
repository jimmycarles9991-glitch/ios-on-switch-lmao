// SPDX-License-Identifier: MIT
// What the compiler does when it cannot serve (compiler_service.mm), so a host test holds the rules. Both launchers start a compiler again after a failing exit and
// leave one that exits 0 (launchd's KeepAlive SuccessfulExit false on iOS 27; the BlueTool stand-in on iOS 17, whose last
// start sets SWITCH_SHADER_COMPILER_LAST_START), and the kernel hands the requests a compiler held when it went to the next
// one. The kernel counts an announced compiler that went without registering as a loss and stops waiting at the third in
// a row (SwitchGpuShaderCompilerPolicy.h kLosses).
#pragma once
#include <IOKit/IOReturn.h>
#include <cstdint>

namespace switch_compiler {

enum class StartFailure : uint8_t {
    Structural,     // every start this boot meets it again: the installed pack, this binary's open or registration
    Transient,      // a later start may not: the GPU connection, the store's file, the kernel's memory
    AnotherServes,  // another compiler holds the registration
};

enum class Ending : uint8_t {
    Decline,  // no compiler comes this boot: the kernel is told, so no client waits, and the compiler exits 0
    Restart,  // exit 1 without declining: the launcher starts a fresh compiler
    Leave,    // exit 0, leaving the kernel to the compiler that serves
};

// `restarts`: the launcher starts it again after a failing exit; `kernelGaveUp`: the kernel stopped waiting for a
// compiler before this start (TegraGM20BCompilerDeclined after the losses in a row), so one more start only loops
constexpr Ending ending(StartFailure failure, bool restarts, bool kernelGaveUp)
{
    if (failure == StartFailure::AnotherServes)
        return Ending::Leave;
    return failure == StartFailure::Transient && restarts && !kernelGaveUp ? Ending::Restart : Ending::Decline;
}

// IOServiceOpen's answer for the GPU's service: one that refuses this task or the open type does so at every start
constexpr StartFailure connectFailure(int kr)
{
    return kr == kIOReturnNotPrivileged || kr == kIOReturnNotPermitted || kr == kIOReturnUnsupported ||
                   kr == kIOReturnBadArgument
               ? StartFailure::Structural
               : StartFailure::Transient;
}

// The kernel's answer to Register (SwitchGpuShaderCompiler.cpp registerCompiler)
constexpr StartFailure registrationFailure(int kr)
{
    if (kr == kIOReturnExclusiveAccess)
        return StartFailure::AnotherServes;
    return kr == kIOReturnNotPrivileged || kr == kIOReturnBadArgument || kr == kIOReturnUnsupported
               ? StartFailure::Structural
               : StartFailure::Transient;
}

}  // namespace switch_compiler
