// XusbPortTransports.h against a model of IOAccessoryManager's IOPort that, as 24B119's does, refuses a second transport
// of a type at any index ("Multiple transports of this type are not allowed on this port!"). Through a host session's
// attach, SuperSpeed reset owed and taken, lane down and up and detach, the USB 2.0 transport is added once and removed
// once; a role swap replaces each type by a removal then an addition, and nothing is ever refused.
#include "drivers/SwitchUSB/XusbPortTransports.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace {

struct Transport {
    uint32_t type;
    xusb_port_role role;
};

struct Entry {
    Transport *transport;
    uint32_t index;
};

struct Port {
    std::map<uint32_t, Entry> held;  // by type
    std::vector<std::string> log;
    unsigned refused = 0, wrongIndex = 0, alive = 0;
};

Port *gPort;

std::string name(uint32_t type, xusb_port_role role)
{
    return std::string(type == xport::kUSB3 ? "USB3 " : "USB2 ") + xusb_port_role_name(role);
}

void *create(uint32_t type, xusb_port_role role)
{
    ++gPort->alive;
    return new Transport{type, role};
}

bool add(void *transport, uint32_t type, uint32_t index)
{
    auto *t = static_cast<Transport *>(transport);
    if (gPort->held.contains(type)) {
        ++gPort->refused;
        return false;
    }
    gPort->held[type] = Entry{t, index};
    gPort->log.push_back("+" + name(type, t->role));
    return true;
}

void remove(uint32_t type, uint32_t index)
{
    const auto found = gPort->held.find(type);
    ASSERT_NE(found, gPort->held.end());
    gPort->wrongIndex += found->second.index != index ? 1 : 0;
    gPort->log.push_back("-" + name(type, found->second.transport->role));
    gPort->held.erase(found);
}

void release(void *transport)
{
    --gPort->alive;
    delete static_cast<Transport *>(transport);
}

void note(const char *, uint32_t, uint32_t, xusb_port_role, const char *)
{}

constexpr xport::Ops kOps = {&create, &add, &remove, &release, &note};

class XusbPortTransports : public ::testing::Test {
protected:
    Port port;
    xport::Transports transports{kOps};

    void SetUp() override
    {
        gPort = &port;
    }

    void TearDown() override
    {
        transports.follow(XUSB_PORT_EMPTY, false, true, "detach");
        transports.dropRetired();
        EXPECT_TRUE(port.held.empty());
        EXPECT_EQ(port.alive, 0u) << "every transport created is released";
        EXPECT_EQ(port.refused, 0u) << "the port never holds two of a type";
        EXPECT_EQ(port.wrongIndex, 0u);
    }

    const Transport *held(uint32_t type) const
    {
        const auto found = port.held.find(type);
        return found == port.held.end() ? nullptr : found->second.transport;
    }

    std::vector<std::string> since(size_t mark) const
    {
        return {port.log.begin() + static_cast<std::ptrdiff_t>(mark), port.log.end()};
    }
};

// The device boot of 2026-11-14: the lane up at attach, the reset owed, then taken, a controller reset owing it again,
// the lane down and up, the detach. The USB 2.0 transport the root port's device sits on stays throughout.
TEST_F(XusbPortTransports, HostSessionNeverTouchesTheUsb2Transport)
{
    transports.follow(XUSB_PORT_HOST, true, false, "attach");
    const Transport *usb2 = held(xport::kUSB2);
    ASSERT_NE(usb2, nullptr);
    EXPECT_EQ(usb2->role, XUSB_PORT_HOST);
    EXPECT_EQ(held(xport::kUSB3), nullptr) << "no USB 3 transport while the reset is owed";

    transports.follow(XUSB_PORT_HOST, true, true, "SuperSpeed reset taken");
    ASSERT_NE(held(xport::kUSB3), nullptr);
    EXPECT_EQ(held(xport::kUSB2), usb2);

    transports.follow(XUSB_PORT_HOST, true, false, "SuperSpeed reset owed");
    EXPECT_EQ(held(xport::kUSB3), nullptr);
    transports.follow(XUSB_PORT_HOST, true, true, "SuperSpeed reset taken");
    transports.follow(XUSB_PORT_HOST, false, true, "lane down");
    EXPECT_EQ(held(xport::kUSB3), nullptr);
    transports.follow(XUSB_PORT_HOST, true, true, "lane up");
    EXPECT_NE(held(xport::kUSB3), nullptr);
    EXPECT_EQ(held(xport::kUSB2), usb2);

    const size_t detach = port.log.size();
    transports.follow(XUSB_PORT_EMPTY, false, true, "detach");
    EXPECT_EQ(since(detach), (std::vector<std::string>{"-USB3 host", "-USB2 host"}));
    EXPECT_EQ(std::count(port.log.begin(), port.log.end(), "+USB2 host"), 1);
    EXPECT_EQ(std::count(port.log.begin(), port.log.end(), "-USB2 host"), 1);
}

// A swap changes each live type's data role by a removal then an addition in the one call; a USB 3 transport the new
// role does not want just goes.
TEST_F(XusbPortTransports, RoleSwapRemovesThenAdds)
{
    transports.follow(XUSB_PORT_DEVICE, false, true, "attach");
    EXPECT_EQ(port.log, (std::vector<std::string>{"+USB2 device"}));

    size_t mark = port.log.size();
    transports.follow(XUSB_PORT_HOST, true, false, "role change");
    EXPECT_EQ(since(mark), (std::vector<std::string>{"-USB2 device", "+USB2 host"}));
    transports.follow(XUSB_PORT_HOST, true, true, "SuperSpeed reset taken");

    mark = port.log.size();
    transports.follow(XUSB_PORT_DEVICE, false, true, "role change");
    EXPECT_EQ(since(mark), (std::vector<std::string>{"-USB3 host", "-USB2 host", "+USB2 device"}));
    EXPECT_EQ(transports.role(), XUSB_PORT_DEVICE);
}

// Leaving the host role with a device on the root port keeps its transports; a new attach before that device has gone
// takes the retired transport of a type out before adding its own.
TEST_F(XusbPortTransports, RetiredTransportsGoBeforeTheirTypeComesBack)
{
    transports.follow(XUSB_PORT_HOST, true, true, "attach");
    transports.retire("detach");
    EXPECT_EQ(port.held.size(), 2u);
    EXPECT_TRUE(transports.holdsRetired());
    EXPECT_EQ(transports.role(), XUSB_PORT_EMPTY);

    const size_t mark = port.log.size();
    transports.follow(XUSB_PORT_DEVICE, false, true, "attach");
    EXPECT_EQ(since(mark), (std::vector<std::string>{"-USB2 host", "+USB2 device"}));
    transports.dropRetired();
    EXPECT_FALSE(transports.holdsRetired());
    EXPECT_EQ(port.held.size(), 1u);
}

TEST_F(XusbPortTransports, DropRetiredClearsBothTypesBeforeDeviceAttach)
{
    transports.follow(XUSB_PORT_HOST, true, true, "attach");
    transports.retire("detach");
    ASSERT_TRUE(transports.holdsRetired());
    const size_t mark = port.log.size();
    transports.dropRetired();
    transports.follow(XUSB_PORT_DEVICE, false, true, "attach");
    EXPECT_EQ(since(mark), (std::vector<std::string>{"-USB3 host", "-USB2 host", "+USB2 device"}));
    EXPECT_FALSE(transports.holdsRetired());
    EXPECT_EQ(held(xport::kUSB3), nullptr);
    ASSERT_NE(held(xport::kUSB2), nullptr);
    EXPECT_EQ(held(xport::kUSB2)->role, XUSB_PORT_DEVICE);
}

// A refused addition leaves no transport behind and is tried again on the next pass.
TEST_F(XusbPortTransports, RefusedAdditionIsReleasedAndRetried)
{
    Transport other{xport::kUSB2, XUSB_PORT_DEVICE};
    port.held[xport::kUSB2] = Entry{&other, 0};
    transports.follow(XUSB_PORT_HOST, false, true, "attach");
    EXPECT_EQ(port.refused, 1u);
    EXPECT_EQ(port.alive, 0u);
    EXPECT_EQ(transports.role(), XUSB_PORT_EMPTY);
    port.refused = 0;
    port.held.clear();
    transports.follow(XUSB_PORT_HOST, false, true, "attach");
    EXPECT_EQ(transports.role(), XUSB_PORT_HOST);
}

}  // namespace
