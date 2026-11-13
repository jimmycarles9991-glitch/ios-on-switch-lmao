import HWTestCore

/// The display copies a scanout frame on the CPU only while VIC is not up: the GPU's copy engine takes a frame window
/// A cannot scan in place, VIC one the copy engine declines. kern.switch_scanout_* (debug kernels) count since boot the
/// frames VIC copied, the CPU copied and none took, and say whether VIC is up now.
struct ScanoutRoutesTest: HWTest {
    let id = "display.scanout_routes"
    let name = "No scanout frame is copied on the CPU while VIC is up"
    let subsystem = Subsystem.compositor

    static let counters = ["vic", "cpu", "unshown", "vic_up"]

    func run(_ context: TestContext) async throws -> Outcome {
        var values: [String: Int] = [:]
        var unread: Int32 = 0
        for name in Self.counters {
            switch KernelSysctl.integer("kern.switch_scanout_" + name, as: Int32.self) {
            case .value(let value): values[name] = Int(value)
            case .failed(let error): if unread == 0 { unread = error }
            }
        }
        var metrics = Metrics()
        guard unread == 0 else {
            metrics.set("sysctl", KernelSysctl.code(unread))
            return .skip(KernelSysctl.unreadable("kern.switch_scanout_*", unread), metrics)
        }
        for name in Self.counters { metrics.set(name, values[name] ?? -1) }
        guard values["vic_up"] == 1 else {
            return .skip("VIC is not up: the CPU copies scanout frames by design", metrics)
        }
        return .judged(
            [
                ("no frame copied on the CPU since boot (cpu 0)", values["cpu"] == 0),
                ("no frame left unshown since boot (unshown 0)", values["unshown"] == 0),
            ], metrics)
    }
}
