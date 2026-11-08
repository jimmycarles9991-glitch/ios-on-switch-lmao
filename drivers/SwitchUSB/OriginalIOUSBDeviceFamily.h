// SPDX-License-Identifier: BSD-2-Clause-Patent
// The original IOUSBDeviceFamily classes SwitchXUDC uses besides IOUSBDeviceController (kits/SwitchABI, which binds
// their members for the selected IPSW kind). These layouts are the same in every kind so far (the kind tables' hints
// show the loads).
#pragma once
#include "OriginalIOUSBDeviceController.h"

// EP0 state machine (OSObject, 0xa0 bytes). States at +12: 1 idle, 2 setup
// received, 3 IN request, 4 OUT request with data, 5/6 OUT data stage,
// 7 waiting for the device's status (IN zero-length), 8 IN data done and
// waiting for the host's status (OUT zero-length), 9 stalled.
class IOUSBDeviceControlRequest {
public:
    void inputSetupPacket(uint8_t *packet);
    void processSetupPacket();
    void inputDataPacket(uint8_t *bytes, uint64_t length);

    int state() const
    {
        return *reinterpret_cast<const volatile int *>(reinterpret_cast<const unsigned char *>(this) + 12);
    }

    uint16_t setupLength() const
    {
        const unsigned char *setup = reinterpret_cast<const unsigned char *>(this) + 16;
        return static_cast<uint16_t>(setup[6] | static_cast<unsigned>(setup[7]) << 8);
    }

    IOUSBDeviceControlRequest() = delete;
};

// Function interface and pipe accessors used to assign endpoint addresses.
class IOUSBDeviceInterface {
public:
    int getAlternateSettingCount() const;
    int getPipeCountForAlternateSetting(uint8_t alternate);
    class IOUSBDevicePipe *getPipeForAlternateSetting(int index, uint8_t alternate);

    IOUSBDeviceInterface() = delete;
};

class IOUSBDevicePipe {
public:
    int getDirection();  // 0 OUT, 1 IN
    IOUSBDevicePipe() = delete;
};
