// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace swlan {

enum class SwitchAwdlRxActionKind : uint8_t { Short, BlockAck, Psf, Mif, Vendor, Other, Count };

struct SwitchAwdlRxAction {
    SwitchAwdlRxActionKind kind = SwitchAwdlRxActionKind::Short;
    uint16_t fc = 0;
    uint16_t sequenceControl = 0;
    unsigned headerBytes = 0;
    uint8_t source[6] = {};
    int category = -1;
    int blockAckAction = -1;
    int oui = -1;
    int appleType = -1;
    int version = -1;
    int subtype = -1;
};

inline bool switch_awdl_rx_action(const uint8_t *frame, size_t lengthWithoutFCS, SwitchAwdlRxAction &out)
{
    if (!frame || lengthWithoutFCS < 24 || frame[0] != 0xd0)
        return false;
    out = {};
    out.fc = frame[0] | (frame[1] << 8);
    out.sequenceControl = frame[22] | (frame[23] << 8);
    for (size_t i = 0; i < sizeof out.source; ++i)
        out.source[i] = frame[10 + i];
    out.headerBytes = (out.fc & 0x8000) ? 28 : 24;
    if (lengthWithoutFCS <= out.headerBytes)
        return true;

    const uint8_t *body = frame + out.headerBytes;
    const size_t bytes = lengthWithoutFCS - out.headerBytes;
    out.category = body[0];
    if (out.category == 3) {
        if (bytes >= 2) {
            out.kind = SwitchAwdlRxActionKind::BlockAck;
            out.blockAckAction = body[1];
        }
    } else if (out.category != 127) {
        out.kind = SwitchAwdlRxActionKind::Other;
    } else if (bytes >= 4) {
        out.oui = (body[1] << 16) | (body[2] << 8) | body[3];
        out.kind = SwitchAwdlRxActionKind::Vendor;
        if (out.oui == 0x0017f2 && bytes >= 5) {
            out.appleType = body[4];
            if (out.appleType == 8) {
                if (bytes >= 6)
                    out.version = body[5];
                if (bytes >= 7) {
                    out.subtype = body[6];
                    if (out.subtype == 0)
                        out.kind = SwitchAwdlRxActionKind::Psf;
                    else if (out.subtype == 3)
                        out.kind = SwitchAwdlRxActionKind::Mif;
                }
            }
        }
    }
    return true;
}

struct SwitchAwdlRxActionBudget {
    bool take(const SwitchAwdlRxAction &action, bool unexpectedResult = false)
    {
        const bool retry = (action.fc & 0x0800) != 0;
        const bool exceptional = (action.fc & 0x8000) != 0 || unexpectedResult;
        auto &pool = samples[static_cast<size_t>(action.kind)][retry][exceptional];
        if (pool.count == 2)
            return false;
        for (unsigned entry = 0; entry < pool.count; ++entry) {
            size_t i = 0;
            while (i < sizeof action.source && pool.sources[entry][i] == action.source[i])
                ++i;
            if (i == sizeof action.source)
                return false;
        }
        for (size_t i = 0; i < sizeof action.source; ++i)
            pool.sources[pool.count][i] = action.source[i];
        ++pool.count;
        return true;
    }

private:
    struct Pool {
        uint8_t count;
        uint8_t sources[2][6];
    };
    Pool samples[static_cast<size_t>(SwitchAwdlRxActionKind::Count)][2][2] = {};
};

}  // namespace swlan
