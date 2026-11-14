// SPDX-License-Identifier: MIT
// The raw path's order after the process's other driver (RawMetalSubmit.mm): on the GPU through the kernel's Order, on the
// CPU only with a kernel that has no Order. A command buffer whose Order the kernel refused fails; the next is ordered afresh.
#pragma once
#include <mach/kern_return.h>
#include <cstdint>

namespace raw {

// Device::orderAfterOthers's answer when it has no channel to order (none made yet, or its reopen failed). The kernel's Order
// answers IOReturn codes only, so this one is never the kernel's.
constexpr int kOrderNoChannel = KERN_FAILURE;

enum class OrderAnswer {
    ordered,
    noChannel,  // nothing raw can be submitted now: no reason to order on the CPU
    refused,
};

constexpr OrderAnswer orderAnswer(int kr)
{
    return kr == KERN_SUCCESS      ? OrderAnswer::ordered
           : kr == kOrderNoChannel ? OrderAnswer::noChannel
                                   : OrderAnswer::refused;
}

}  // namespace raw
