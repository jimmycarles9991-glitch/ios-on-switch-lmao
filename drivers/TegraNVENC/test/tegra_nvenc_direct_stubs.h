// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel types TegraNVENCDirect.cpp's headers name, for a host build beside the real ownership code, whose
// stand-ins (TegraGM20B/test's coherency kernel) give IOLock, IONew and IOLog.
#pragma once
#include <cstddef>
#include <cstdint>

using IOReturn = int32_t;
using IOOptionBits = uint32_t;
using task_t = void *;

class OSObject;

class IOMemoryDescriptor {};

class IOBufferMemoryDescriptor : public IOMemoryDescriptor {};
