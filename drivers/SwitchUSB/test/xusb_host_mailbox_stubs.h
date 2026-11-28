// Extra stub types so XusbHost.cpp compiles directly against xusb_register_model on the host.
#ifndef SWITCH_TEST_XUSB_HOST_MAILBOX_STUBS_H
#define SWITCH_TEST_XUSB_HOST_MAILBOX_STUBS_H
#include "xusb_kernel_stubs.h"

#include <cstring>

using IOOptionBits = uint32_t;
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
using vm_size_t = size_t;
using vm_offset_t = uintptr_t;

constexpr IOOptionBits kIOMapWriteCombineCache = 0x400;
constexpr IOOptionBits kIOMemoryTypeMask = 0xf;
constexpr IOOptionBits kIOMemoryTypeVirtual64 = 0x1;
constexpr IOOptionBits kIOMemoryBufferPageable = 0x10000;
constexpr IOOptionBits _kIOMemorySourceSegment = 0x2;
constexpr IOOptionBits kIOMemoryMapperNone = 0x4;

#define OSDynamicCast(cls, obj) dynamic_cast<cls *>(obj)

class OSObject {
public:
    virtual ~OSObject() = default;
};

class IOMemoryDescriptor : public OSObject {
public:
    virtual IOOptionBits getFlags() const
    {
        return 0;
    }

    virtual addr64_t getPhysicalSegment(IOByteCount, IOByteCount *, IOOptionBits = 0)
    {
        return 0;
    }

    virtual IOByteCount readBytes(IOByteCount, void *, IOByteCount)
    {
        return 0;
    }

    virtual IOByteCount writeBytes(IOByteCount, const void *, IOByteCount)
    {
        return 0;
    }
};

class IOGeneralMemoryDescriptor : public IOMemoryDescriptor {};
class IOBufferMemoryDescriptor : public IOGeneralMemoryDescriptor {};

#endif
