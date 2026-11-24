// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <cstdint>

namespace switch_wlan_awdl_role_fixture {
constexpr uint64_t kVirtualBytes = 0x006b6e694c726941ULL;
#if defined(SWITCH_IPSW_KIND_24B119)
constexpr uint64_t kBytes = 0x006b6e694c726941ULL;
#elif defined(SWITCH_IPSW_KIND_21H461)
constexpr uint64_t kBytes = 0x000000004c445741ULL;
#endif
}
