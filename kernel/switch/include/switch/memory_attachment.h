// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
class IOMemoryDescriptor;
class OSObject;
struct IOMemoryDescriptorReserved;

class SwitchMemoryAttachment {
public:
    static IOMemoryDescriptorReserved *prepare(IOMemoryDescriptor *memory);
    // The caller retains memory. Representation changes advance this word under full logical CPU WRITE ownership.
    static volatile uint64_t *generation(IOMemoryDescriptor *memory);
    // The caller retains memory; returned copies retain their object independently.
    static OSObject *copy(IOMemoryDescriptor *memory);
    // Replacement retains the new object and releases the displaced object outside the attachment lock.
    static bool replace(IOMemoryDescriptor *memory, OSObject *expected, OSObject *replacement);
    // The GPU driver's own record of this memory, in a slot of its own with the same rules; the descriptor's end
    // releases it, so the record's free is where the GPU driver learns the memory is gone.
    static OSObject *copyGpu(IOMemoryDescriptor *memory);
    static bool replaceGpu(IOMemoryDescriptor *memory, OSObject *expected, OSObject *replacement);
    static void clear(IOMemoryDescriptorReserved *reserved);
    // Successful reservations retain the descriptor until their matching leave. Nested CPU access remains admitted
    // while a publisher waits; an exclusive reservation admits neither CPU access nor another publisher.
    static int enterCpu(IOMemoryDescriptor *memory, bool avoid);
    static void leaveCpu(IOMemoryDescriptor *memory);
    static uint32_t cpuUsers(IOMemoryDescriptor *memory);
    static int enterExclusive(IOMemoryDescriptor *memory, bool avoid);
    static void leaveExclusive(IOMemoryDescriptor *memory);

private:
    static OSObject *copySlot(IOMemoryDescriptor *memory, bool gpu);
    static bool replaceSlot(IOMemoryDescriptor *memory, bool gpu, OSObject *expected, OSObject *replacement);
};
