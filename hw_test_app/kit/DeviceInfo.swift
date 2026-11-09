import Foundation
import HWTestCore
import MachO
import UIKit

enum DeviceInfo {
    @MainActor
    static func current() -> [String: String] {
        let device = UIDevice.current
        return [
            "model": device.model,
            "name": device.name,
            "system": "\(device.systemName) \(device.systemVersion)",
            "build": sysctl("kern.osversion"),
            "machine": sysctl("hw.machine"),
            "board": sysctl("hw.model"),
            "app": Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "",
        ]
    }

    /// What a report needs to say which system, boot and app it measured; the Mac adds the deployed commit
    /// (tools/hwtest), which the device cannot see. nak_debug: NAK_DEBUG changes how NAK compiles every shader of the
    /// process, so tools/hwtest does not compare GPU results across a change in it.
    @MainActor
    static func identity(ios: Int) -> [String: String] {
        var identity = current()
        identity["ios"] = String(ios)
        identity["kernel"] = sysctl("kern.version")
        identity["kernel_uuid"] = sysctl("kern.uuid")
        identity["boot_uuid"] = bootUUID
        identity["boot_time"] = bootTime.map(Report.timestamp) ?? ""
        identity["uptime_s"] = String(Int(ProcessInfo.processInfo.systemUptime))
        identity["bundle_id"] = Bundle.main.bundleIdentifier ?? ""
        identity["app_uuid"] = mainImageUUID()
        identity["nak_debug"] = ProcessInfo.processInfo.environment["NAK_DEBUG"] ?? ""
        return identity
    }

    static var bootUUID: String { sysctl("kern.bootsessionuuid") }

    static var bootTime: Date? {
        var time = timeval()
        var size = MemoryLayout<timeval>.size
        guard sysctlbyname("kern.boottime", &time, &size, nil, 0) == 0 else { return nil }
        return Date(timeIntervalSince1970: Double(time.tv_sec) + Double(time.tv_usec) / 1e6)
    }

    /// The app executable's LC_UUID: which build of the app ran.
    static func mainImageUUID() -> String {
        guard let header = _dyld_get_image_header(0) else { return "" }
        var command = UnsafeRawPointer(header).advanced(by: MemoryLayout<mach_header_64>.size)
        for _ in 0..<header.pointee.ncmds {
            let load = command.load(as: load_command.self)
            if load.cmd == UInt32(LC_UUID) {
                return UUID(uuid: command.load(as: uuid_command.self).uuid).uuidString
            }
            command = command.advanced(by: Int(load.cmdsize))
        }
        return ""
    }

    static func sysctl(_ name: String) -> String {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "" }
        var bytes = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &bytes, &size, nil, 0) == 0 else { return "" }
        return String(cString: bytes)
    }

    static func sysctlInt(_ name: String) -> Int {
        var value: Int64 = 0
        var size = MemoryLayout<Int64>.size
        guard sysctlbyname(name, &value, &size, nil, 0) == 0 else { return 0 }
        return size == 4 ? Int(Int32(truncatingIfNeeded: value)) : Int(value)
    }
}
