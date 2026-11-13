// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stand-ins for the use-count test: an IOSurface as OSObject with its flag byte where 24B119 keeps it.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)

namespace OriginalIOSurface {
constexpr unsigned long useCountFlags = 0x3d2;
}

class OSObject {
public:
    int refs = 1;
    int uses = 0;
    unsigned char bytes[0x400] = {};

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

inline unsigned usesChanged;

inline void IOLog(const char *format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
}

extern "C" inline void switch_surface_use_increment(OSObject *surface, uint32_t category)
{
    if (category == 0)
        ++surface->uses;
    ++usesChanged;
}

extern "C" inline void switch_surface_use_decrement(OSObject *surface, uint32_t category)
{
    if (category == 0 && surface->uses > 0)
        --surface->uses;
    ++usesChanged;
}

extern "C" inline uint32_t switch_surface_use_count(OSObject *surface)
{
    return uint32_t(surface->uses);
}
