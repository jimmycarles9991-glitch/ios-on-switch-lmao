// SPDX-License-Identifier: MIT
// The host imports one submission touches, as Exec's surface list names them (TegraGM20BABI.h method 17): each once, written
// if any of its uses writes it. More than the list holds makes it TegraGM20BExecSurfacesAll, and the Exec then runs as one
// with no list.
#pragma once
#include <cstddef>
#include <cstdint>
#include "TegraGM20BABI.h"

namespace raw {

class SurfaceList {
public:
    void clear()
    {
        list_.count = 0;
        list_.flags = 0;
        last_ = 0;
    }

    void add(uint64_t handle, bool write)
    {
        const uint32_t access = write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead;
        if (last_ < list_.count && list_.uses[last_].handle == handle) {
            list_.uses[last_].access |= access;
            return;
        }
        for (uint32_t i = 0; i < list_.count; i++)
            if (list_.uses[i].handle == handle) {
                list_.uses[i].access |= access;
                last_ = i;
                return;
            }
        if (list_.count == TegraGM20BMaxExecSurfaces) {
            list_.flags |= TegraGM20BExecSurfacesAll;
            return;
        }
        last_ = list_.count;
        list_.uses[list_.count++] = {handle, access, 0};
    }

    // The record for `channel`, `bytes` long: it ends after its uses.
    const TegraGM20BAbiExecSurfaces &record(uint32_t channel, size_t &bytes)
    {
        list_.channel = channel;
        bytes = offsetof(TegraGM20BAbiExecSurfaces, uses) + list_.count * sizeof list_.uses[0];
        return list_;
    }

private:
    TegraGM20BAbiExecSurfaces list_ = {};
    uint32_t last_ = 0;  // the use added last: a texture bound draw after draw finds itself first
};

}  // namespace raw
