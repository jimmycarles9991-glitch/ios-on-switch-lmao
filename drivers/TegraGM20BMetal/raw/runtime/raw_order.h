// SPDX-License-Identifier: MIT
// The raw path's order after the process's other driver (RawMetalSubmit.mm): on the GPU through the kernel's Order, on the
// CPU only after the kernel refused one, and then only until the kernel makes the raw channel again.
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

// After a raw submission went through: whether a refused Order gives way to GPU order again (`reopens`: the channels the
// kernel made since the device opened, `refusedAt` their count when it refused)
constexpr bool orderReturns(bool gpuOrder, bool canOrder, uint64_t reopens, uint64_t refusedAt)
{
    return !gpuOrder && canOrder && reopens != refusedAt;
}

}  // namespace raw
