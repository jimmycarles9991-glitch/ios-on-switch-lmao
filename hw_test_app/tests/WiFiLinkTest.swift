import Darwin
import Foundation
import HWTestCore

/// en0 as the kernel's interface list has it (getifaddrs): up and running, its IPv4 address, the link rate the
/// Wi-Fi driver reports and its packet and error counters; and the association as the driver answers wifid's
/// Apple80211 requests (state and the joined network's name).
struct WiFiLinkTest: HWTest {
    let id = "net.wifi_link"
    let name = "Wi-Fi interface: link, address, rate and errors"
    let subsystem = Subsystem.connectivity

    struct Interface {
        var flags: UInt32 = 0
        var ipv4 = ""
        var data: if_data?
    }

    static func interface(_ name: String) -> Interface? {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0, let first = list else { return nil }
        defer { freeifaddrs(list) }
        var found: Interface?
        for entry in sequence(first: first, next: { $0.pointee.ifa_next }) {
            guard String(cString: entry.pointee.ifa_name) == name, let address = entry.pointee.ifa_addr else {
                continue
            }
            var interface = found ?? Interface()
            interface.flags = entry.pointee.ifa_flags
            if address.pointee.sa_family == UInt8(AF_LINK), let data = entry.pointee.ifa_data {
                interface.data = data.load(as: if_data.self)
            } else if address.pointee.sa_family == UInt8(AF_INET) {
                var host = [CChar](repeating: 0, count: Int(NI_MAXHOST))
                getnameinfo(
                    address, socklen_t(address.pointee.sa_len), &host, socklen_t(host.count), nil, 0, NI_NUMERICHOST)
                interface.ipv4 = String(cString: host)
            }
            found = interface
        }
        return found
    }

    /// An Apple80211 get on en0 as wifid sends it (SIOCGA80211 with a 40-byte apple80211req): the answer's length
    /// (the driver's req_len), or nil with errno.
    static func apple80211(_ type: Int32, into buffer: inout [UInt8]) -> (length: Int?, error: Int32) {
        let fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return (nil, errno) }
        defer { close(fd) }
        var request = [UInt8](repeating: 0, count: 40)
        let capacity = buffer.count
        return buffer.withUnsafeMutableBytes { data in
            request.withUnsafeMutableBytes { raw in
                for (index, byte) in "en0".utf8.enumerated() { raw[index] = byte }
                raw.storeBytes(of: type, toByteOffset: 16, as: Int32.self)
                raw.storeBytes(of: UInt32(capacity), toByteOffset: 24, as: UInt32.self)
                raw.storeBytes(of: UInt64(UInt(bitPattern: data.baseAddress)), toByteOffset: 32, as: UInt64.self)
                guard ioctl(fd, 0xc028_69c9, raw.baseAddress!) == 0 else { return (nil, errno) }
                return (Int(raw.load(fromByteOffset: 24, as: UInt32.self)), 0)
            }
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        guard let wifi = Self.interface("en0") else { return .fail("no en0 in getifaddrs") }
        var metrics = Metrics()
        let up = wifi.flags & UInt32(IFF_UP) != 0
        let running = wifi.flags & UInt32(IFF_RUNNING) != 0
        metrics.set("up", up)
        metrics.set("running", running)
        metrics.set("ipv4", wifi.ipv4.isEmpty ? "-" : wifi.ipv4)
        if let data = wifi.data {
            metrics.set("link_mbps", Double(data.ifi_baudrate) / 1e6, digits: 1)
            metrics.set("packets_in", Int(data.ifi_ipackets))
            metrics.set("packets_out", Int(data.ifi_opackets))
            metrics.set("errors_in", Int(data.ifi_ierrors))
            metrics.set("errors_out", Int(data.ifi_oerrors))
        }
        var state = [UInt8](repeating: 0, count: 8)
        let stateAnswer = Self.apple80211(13, into: &state)
        var ssid = [UInt8](repeating: 0, count: 32)
        let ssidAnswer = Self.apple80211(1, into: &ssid)
        let driverState = stateAnswer.length == nil ? -1 : Int(state[4])
        let ssidLength = ssidAnswer.length ?? 0
        metrics.set("driver_state", driverState)
        metrics.set("ssid_len", ssidLength)
        if stateAnswer.length == nil { metrics.set("state_errno", Int(stateAnswer.error)) }
        if ssidAnswer.length == nil { metrics.set("ssid_errno", Int(ssidAnswer.error)) }
        guard !wifi.ipv4.isEmpty else {
            return .skip("en0 has no IPv4 address: not associated with a network", metrics)
        }
        return .judged(
            [
                ("en0 up and running", up && running),
                ("the driver reports a link rate", (wifi.data?.ifi_baudrate ?? 0) > 0),
                ("the driver is associated (APPLE80211_IOC_STATE 4)", driverState == 4),
                ("the driver names the joined network (1-32 bytes)", (1...32).contains(ssidLength)),
            ], metrics)
    }
}
