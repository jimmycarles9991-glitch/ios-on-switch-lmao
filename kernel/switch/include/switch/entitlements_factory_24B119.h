// SPDX-License-Identifier: GPL-2.0-only
#ifndef SWITCH_ENTITLEMENTS_FACTORY_24B119_H
#define SWITCH_ENTITLEMENTS_FACTORY_24B119_H

#define SWITCH_AMFI_FACTORY_DELTA 0xa58c
#define SWITCH_AMFI_IDENTITY_DELTA 0xa708
static const uint32_t switch_amfi_anchor_words[] = {0xd503245f, 0xd503237f, 0xa9bf7bfd, 0x910003fd};
static const uint32_t switch_amfi_factory_words[] = {
    0xd503245f, 0xd503237f, 0xd10343ff, 0xa90a57f6,
    0xa90b4ff4, 0xa90c7bfd, 0x910303fd, 0xaa0103e4,
};
static const uint32_t switch_amfi_identity_words[] = {0xd503245f, 0x17fffe55};

#endif
