// Host stand-ins for what drivers/SwitchWLAN/SwitchWLANNet.cpp takes from the kernel and from IONetworkingFamily, for
// switch_wlan_net_test.cpp. BUILD.bazel points the kernel's headers and the generated Original*.h here (host_shims).
// mbufs are host allocations with a byte counter on every copy out of or into them, so the test can tell how many
// times each frame was copied. The family's interface is the test's model of IOEthernetInterface's queues.
#ifndef SWITCH_TEST_SWITCH_WLAN_NET_STUBS_H
#define SWITCH_TEST_SWITCH_WLAN_NET_STUBS_H
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

using UInt8 = uint8_t;
using UInt16 = uint16_t;
using UInt32 = uint32_t;
using UInt64 = uint64_t;
using SInt32 = int32_t;
using IOReturn = int;
using IOOptionBits = uint32_t;
using errno_t = int;

constexpr IOReturn kIOReturnSuccess = 0;
constexpr IOReturn kIOReturnNoResources = static_cast<IOReturn>(0xe00002be);
constexpr IOReturn kIOReturnNoFrames = static_cast<IOReturn>(0xe00002d0);
constexpr IOReturn kIOReturnNotAttached = static_cast<IOReturn>(0xe00002d8);

inline std::vector<std::string> gLog;

__attribute__((format(printf, 1, 2))) inline void IOLog(const char *format, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    gLog.emplace_back(line);
}

// The tests are single-threaded: the lock only checks that the driver pairs its calls.
struct IOLock {
    int held = 0;
};

inline IOLock *IOLockAlloc()
{
    return new IOLock;
}

inline void IOLockLock(IOLock *l)
{
    ++l->held;
}

inline void IOLockUnlock(IOLock *l)
{
    --l->held;
}

inline uint64_t mach_absolute_time()
{
    return 0;
}

inline void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns)
{
    *ns = t;
}

class IOService;
class IOWorkLoop;

// ---- mbufs: a packet is a chain of buffers of kMbufRoom bytes, linked to the next packet by nextpkt ------------------
struct mbuf {
    unsigned char room[2048];
    size_t lead = 0, len = 0;
    mbuf *next = nullptr, *nextpkt = nullptr;
    size_t pktlen = 0;
};

using mbuf_t = mbuf *;

enum {
    MBUF_DONTWAIT = 1
};

inline size_t gMbufRoom = 2048;  // a buffer's size (the test makes it small to force chains)
inline size_t gMbufCopied;       // bytes copied out of mbufs (transmit)
inline size_t gMbufLive;         // buffers allocated and not yet freed
inline bool gMbufExhausted;      // the next allocation fails

inline errno_t mbuf_allocpacket(int, size_t length, unsigned *, mbuf_t *out)
{
    if (gMbufExhausted)
        return 12;
    const size_t count = length > gMbufRoom ? (length + gMbufRoom - 1) / gMbufRoom : 1;
    mbuf_t head = nullptr, *at = &head;
    for (size_t i = 0; i < count; ++i, at = &(*at)->next) {
        *at = new mbuf;
        ++gMbufLive;
    }
    *out = head;
    return 0;
}

inline mbuf_t mbuf_next(mbuf_t m)
{
    return m->next;
}

inline size_t mbuf_maxlen(mbuf_t)
{
    return gMbufRoom;
}

inline size_t mbuf_leadingspace(mbuf_t m)
{
    return m->lead;
}

inline void *mbuf_datastart(mbuf_t m)
{
    return m->room;
}

inline void *mbuf_data(mbuf_t m)
{
    return m->room + m->lead;
}

inline errno_t mbuf_setdata(mbuf_t m, void *data, size_t len)
{
    m->lead = static_cast<size_t>(static_cast<unsigned char *>(data) - m->room);
    m->len = len;
    return 0;
}

inline errno_t mbuf_setlen(mbuf_t m, size_t len)
{
    m->len = len;
    return 0;
}

inline void mbuf_pkthdr_setlen(mbuf_t m, size_t len)
{
    m->pktlen = len;
}

inline size_t mbuf_pkthdr_len(mbuf_t m)
{
    return m->pktlen;
}

inline mbuf_t mbuf_nextpkt(mbuf_t m)
{
    return m->nextpkt;
}

inline void mbuf_setnextpkt(mbuf_t m, mbuf_t next)
{
    m->nextpkt = next;
}

inline void mbuf_freem(mbuf_t m)
{
    while (m) {
        mbuf_t n = m->next;
        delete m;
        --gMbufLive;
        m = n;
    }
}

inline int mbuf_freem_list(mbuf_t m)
{
    int n = 0;
    while (m) {
        mbuf_t next = m->nextpkt;
        mbuf_freem(m);
        m = next;
        ++n;
    }
    return n;
}

inline errno_t mbuf_copydata(mbuf_t m, size_t offset, size_t length, void *out)
{
    auto *dst = static_cast<unsigned char *>(out);
    for (; m && length; m = m->next) {
        if (offset >= m->len) {
            offset -= m->len;
            continue;
        }
        const size_t take = m->len - offset < length ? m->len - offset : length;
        memcpy(dst, m->room + m->lead + offset, take);
        gMbufCopied += take;
        dst += take;
        length -= take;
        offset = 0;
    }
    return length ? 22 : 0;
}

// ---- the ifnet ------------------------------------------------------------------------------------------------------
struct ifnet {
    unsigned starts = 0, purges = 0;
    uint32_t errorsOut = 0;
};

using ifnet_t = ifnet *;

inline errno_t ifnet_start(ifnet_t ifp)
{
    ++ifp->starts;
    return 0;
}

inline errno_t ifnet_purge(ifnet_t ifp)
{
    ++ifp->purges;
    return 0;
}

inline errno_t ifnet_stat_increment_out(ifnet_t ifp, uint32_t, uint32_t, uint32_t errors)
{
    ifp->errorsOut += errors;
    return 0;
}

// ---- IONetworkingFamily, as SwitchWLANNet.cpp uses it ---------------------------------------------------------------
class IONetworkMedium;
class OSData;

class IOEthernetController {
public:
    struct Report {
        UInt32 status;
        UInt64 speed;
    };

    std::vector<Report> reports;

    bool setLinkStatus(UInt32 status, const IONetworkMedium * = nullptr, UInt64 speed = 0, OSData * = nullptr)
    {
        reports.push_back({status, speed});
        return true;
    }
};

// The interface's two queues: the stack's output queue (frames the test hands it) and the input queue
// (enqueueInputPacket), flushed to `delivered` as the stack's input.
class IOEthernetInterface {
public:
    ifnet ifp;
    bool attached = true;
    std::deque<mbuf_t> output;
    std::vector<mbuf_t> input, delivered;
    unsigned flushes = 0, dequeues = 0;

    IOReturn enqueueInputPacket(mbuf_t m, void * = nullptr, IOOptionBits = 0)
    {
        if (!attached) {
            mbuf_freem(m);
            return kIOReturnNotAttached;
        }
        input.push_back(m);
        return kIOReturnSuccess;
    }

    UInt32 flushInputQueue()
    {
        const auto n = static_cast<UInt32>(input.size());
        if (n)
            ++flushes;
        delivered.insert(delivered.end(), input.begin(), input.end());
        input.clear();
        return n;
    }

    IOReturn dequeueOutputPackets(uint32_t max, mbuf_t *head, mbuf_t *tail = nullptr, uint32_t *count = nullptr,
                                  uint64_t *bytes = nullptr)
    {
        ++dequeues;
        if (output.empty())
            return kIOReturnNoFrames;
        mbuf_t first = nullptr, last = nullptr;
        uint32_t n = 0;
        while (n < max && !output.empty()) {
            mbuf_t m = output.front();
            output.pop_front();
            if (last)
                last->nextpkt = m;
            else
                first = m;
            last = m;
            ++n;
        }
        *head = first;
        if (tail)
            *tail = last;
        if (count)
            *count = n;
        if (bytes)
            *bytes = 0;
        return kIOReturnSuccess;
    }

    ifnet_t getIfnet() const
    {
        return attached ? const_cast<ifnet *>(&ifp) : nullptr;
    }
};

#endif
