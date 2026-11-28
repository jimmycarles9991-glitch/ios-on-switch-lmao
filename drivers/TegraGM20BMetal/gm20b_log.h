// SPDX-License-Identifier: MIT
#pragma once
#include <os/log.h>

namespace gm20b {

inline os_log_t log()
{
    static const os_log_t l = os_log_create("com.brunostjohn.switch.TegraGM20BMetal", "gm20b");
    return l;
}

}  // namespace gm20b
