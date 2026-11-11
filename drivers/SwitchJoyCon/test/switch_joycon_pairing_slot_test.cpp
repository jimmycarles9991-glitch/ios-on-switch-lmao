// The rail's reading of a Joy-Con's pairing log (SwitchJoyConPairingSlot.h): the slot in use found by its magic, its
// checksum computed over the whole slot whatever host capability it holds, and hekate's head-only check, which assumes
// a Switch's or a PC's capability, shown to reject a slot another host made.
#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "../SwitchJoyConPairingSlot.h"

namespace {

using Slot = std::array<uint8_t, SWITCH_JOYCON_SLOT_BYTES>;

constexpr uint8_t kHost[6] = {0x58, 0xb0, 0x3e, 0xe7, 0x07, 0x77};

Slot makeSlot(uint8_t magic, uint8_t caps, const uint8_t host[6] = kHost)
{
    Slot slot{};
    slot[0] = magic;
    slot[1] = SWITCH_JOYCON_SLOT_SIZE;
    for (unsigned i = 0; i < 6; ++i)
        slot[4 + i] = host[i];
    for (unsigned i = 0; i < 16; ++i)
        slot[10 + i] = static_cast<uint8_t>(0x31 * i + 7);
    slot[SWITCH_JOYCON_SLOT_CAPS] = caps;
    uint8_t sum = 0;
    for (unsigned i = 4; i < SWITCH_JOYCON_SLOT_BYTES; ++i)
        sum = static_cast<uint8_t>(sum + slot[i]);
    slot[2] = static_cast<uint8_t>(sum ^ 0x55);
    return slot;
}

// hekate _jc_validate_pairing_info over the 0x1A bytes it reads: the tail taken as zeros plus a Switch's or a PC's
// capability.
bool headOnlyValid(const Slot &slot)
{
    uint8_t sum = 0;
    for (unsigned i = 4; i < SWITCH_JOYCON_SLOT_HEAD; ++i)
        sum = static_cast<uint8_t>(sum + slot[i]);
    return static_cast<uint8_t>((sum + 0x68) ^ 0x55) == slot[2] || static_cast<uint8_t>((sum + 0x08) ^ 0x55) == slot[2];
}

TEST(SwitchJoyConPairingSlot, AWholeSlotChecksOutWhateverItsHostCapability)
{
    for (const uint8_t caps : {0x00, 0x08, 0x48, 0x68})
        EXPECT_TRUE(switch_joycon_slot_valid(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, caps).data())) << int(caps);
}

TEST(SwitchJoyConPairingSlot, TheHeadOnlyCheckRejectsAnotherHostsSlot)
{
    EXPECT_TRUE(headOnlyValid(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x68)));
    EXPECT_TRUE(headOnlyValid(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x08)));
    EXPECT_FALSE(headOnlyValid(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x00)));
    EXPECT_FALSE(headOnlyValid(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x48)));
}

TEST(SwitchJoyConPairingSlot, ACorruptSlotFailsItsChecksum)
{
    Slot slot = makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x00);
    slot[12] ^= 0x01;
    EXPECT_FALSE(switch_joycon_slot_valid(slot.data()));
}

TEST(SwitchJoyConPairingSlot, TheLogIsWalkedByMagic)
{
    EXPECT_EQ(switch_joycon_slot_head_kind(makeSlot(0x00, 0x68).data()), SWITCH_JOYCON_SLOT_EARLIER);
    EXPECT_EQ(switch_joycon_slot_head_kind(makeSlot(SWITCH_JOYCON_SLOT_MAGIC, 0x00).data()), SWITCH_JOYCON_SLOT_IN_USE);
    Slot erased;
    erased.fill(0xff);
    EXPECT_EQ(switch_joycon_slot_head_kind(erased.data()), SWITCH_JOYCON_SLOT_END);
    Slot odd = makeSlot(0x40, 0x68);
    EXPECT_EQ(switch_joycon_slot_head_kind(odd.data()), SWITCH_JOYCON_SLOT_END);
}

}  // namespace
