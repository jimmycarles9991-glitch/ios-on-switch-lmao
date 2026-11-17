import Foundation
import Metal

/// Which Mac a host GPU test ran on: remote build hosts differ in chip and macOS build, and a result that one host
/// gives and another does not names the host by these fields.
enum HostFacts {
    static func fields(_ device: MTLDevice) -> String {
        "mac=\(hostName()) gpu=\(device.name.replacingOccurrences(of: " ", with: "_"))"
            + " arch=\(device.architecture.name) os=\(osBuild())"
    }

    private static func hostName() -> String {
        var name = [CChar](repeating: 0, count: 256)
        guard gethostname(&name, name.count) == 0 else { return "-" }
        return String(cString: name)
    }

    private static func osBuild() -> String {
        var size = 0
        guard sysctlbyname("kern.osversion", nil, &size, nil, 0) == 0, size > 0 else { return "-" }
        var build = [CChar](repeating: 0, count: size)
        guard sysctlbyname("kern.osversion", &build, &size, nil, 0) == 0 else { return "-" }
        return String(cString: build)
    }
}
