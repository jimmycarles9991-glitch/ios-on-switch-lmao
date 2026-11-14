// SPDX-License-Identifier: BSD-2-Clause-Patent
// A primary interrupt filter running on this thread: XNU panics on a mutex, a sleep or an allocation there, and a
// simple lock in a filter spins with interrupts off against whatever thread context holds it. Every stand-in for one of
// those, in every unit of a test, throws while a filter runs. Shared by the units (inline variables).
#pragma once
#include <stdexcept>
#include <string>

inline thread_local int gInFilter = 0;

inline void filterMayNot(const char *what)
{
    if (gInFilter)
        throw std::runtime_error(std::string(what) + " in a primary interrupt filter");
}

struct FilterContext {
    FilterContext()
    {
        ++gInFilter;
    }

    ~FilterContext()
    {
        --gInFilter;
    }

    FilterContext(const FilterContext &) = delete;
    FilterContext &operator=(const FilterContext &) = delete;
};
