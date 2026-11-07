// SPDX-License-Identifier: BSD-2-Clause-Patent
// The orientation device's AOP user client: what CoreMotion's CLOrientationNotifier (backboardd, SpringBoard,
// locationd, audiomxd) reaches through AppleSPULib.plugin's AppleSPUHIDDeviceClass (IOCFPlugInTypes 7ACF5332-...,
// published on the orientation SwitchHIDDevice) to ask the current orientation, which its events only report on a
// change. 21H461 AppleSPULib: Start opens type 'ashd'; Open and Close are selectors 4 and 5, Reset 0, SendCommand 6
// (one scalar, the command; struct in; struct out). CLOrientationNotifier sends command 0x20 twice: {0x04, form factor,
// cover} once (its configuration; only the status is read) and {0x02} for LastOrientation, whose reply is exactly 10
// bytes: an echo of 0x02, the orientation on its events' scale (0 unknown .. 6 face down) and the sample time as a
// little-endian mach_absolute_time (0: nothing yet, which it ignores).
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <string.h>
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "SwitchMotion.h"

namespace {
constexpr uint32_t kSelectorReset = 0, kSelectorOpen = 4, kSelectorClose = 5, kSelectorCommand = 6;
constexpr uint64_t kCommandOrientation = 0x20;
constexpr uint8_t kRequestConfigure = 0x04, kRequestLast = 0x02;
constexpr uint32_t kLastSize = 10, kConfigureSize = 3;
}  // namespace

class SwitchOrientationClient : public IOUserClient {
    OSDeclareDefaultStructors(SwitchOrientationClient)

        static IOReturn refuse(uint32_t selector, uint64_t command, uint8_t request)
    {
        const uint64_t detail = (uint64_t)selector << 32 | command << 8 | request;
        switch_refusal_k(switch_refusal_key_with(switch_refusal_key("orientation", "aop-call"), detail), "orientation",
                         "aop-call", "selector %u command %#llx request %#x has no answer here", selector,
                         (unsigned long long)command, request);
        return kIOReturnUnsupported;
    }

    static IOReturn command(IOExternalMethodArguments *args)
    {
        if (args->scalarInputCount != 1 || !args->structureInput || !args->structureInputSize)
            return kIOReturnBadArgument;
        const uint8_t *in = static_cast<const uint8_t *>(args->structureInput);
        uint8_t *out = static_cast<uint8_t *>(args->structureOutput);
        if (args->scalarInput[0] != kCommandOrientation)
            return refuse(kSelectorCommand, args->scalarInput[0], in[0]);
        if (in[0] == kRequestConfigure) {
            const uint32_t echo = args->structureInputSize < kConfigureSize ? args->structureInputSize : kConfigureSize;
            if (out && args->structureOutputSize >= echo) {
                memcpy(out, in, echo);
                args->structureOutputSize = echo;
            }
            return kIOReturnSuccess;
        }
        if (in[0] != kRequestLast)
            return refuse(kSelectorCommand, args->scalarInput[0], in[0]);
        if (!out || args->structureOutputSize < kLastSize)
            return kIOReturnBadArgument;
        uint8_t orientation = 0;
        uint64_t stamp = 0;
        switch_motion_last_orientation(&orientation, &stamp);
        out[0] = kRequestLast;
        out[1] = orientation;
        for (unsigned i = 0; i < 8; ++i)
            out[2 + i] = (uint8_t)(stamp >> (8 * i));
        args->structureOutputSize = kLastSize;
        return kIOReturnSuccess;
    }

public:
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *,
                            void *) override
    {
        switch (selector) {
        case kSelectorReset:
        case kSelectorOpen:
        case kSelectorClose:
            return kIOReturnSuccess;
        case kSelectorCommand:
            return command(args);
        default:
            return refuse(selector, 0, 0);
        }
    }

    IOReturn clientClose() override
    {
        terminate();
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(SwitchOrientationClient, IOUserClient)

    extern "C" IOReturn switch_motion_orientation_client(IOService *device, task_t task, void *security, uint32_t type,
                                                         IOUserClient **handler)
{
    SwitchOrientationClient *client = OSTypeAlloc(SwitchOrientationClient);
    if (!client || !client->initWithTask(task, security, type) || !client->attach(device)) {
        OSSafeReleaseNULL(client);
        return kIOReturnNoMemory;
    }
    if (!client->start(device)) {
        client->detach(device);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}
