// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw {

enum class AttachmentBarrier : uint8_t {
    None,
    Pixel,
    Shader
};

struct AttachmentWrites {
    uint8_t rop = 0, storage = 0;

    void synchronized(AttachmentBarrier barrier)
    {
        if (barrier != AttachmentBarrier::None)
            rop = 0;
        if (barrier == AttachmentBarrier::Shader)
            storage = 0;
    }
};

struct AttachmentAccess {
    uint8_t ropRead = 0, ropWrite = 0, sampled = 0, storage = 0;

    AttachmentBarrier after(uint8_t ropWritten, uint8_t storageWritten) const
    {
        if ((storage & ropWritten) || ((ropRead | ropWrite | sampled) & storageWritten))
            return AttachmentBarrier::Shader;
        return sampled && ropWritten ? AttachmentBarrier::Pixel : AttachmentBarrier::None;
    }
};

}  // namespace raw
