import Foundation
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
