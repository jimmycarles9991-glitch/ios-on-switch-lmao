import Foundation
import HWTestCore
import UIKit

struct IdentityTest: HWTest {
    let id = "system.identity"
    let name = "Device identity"
    let subsystem = Subsystem.system

    func run(_ context: TestContext) async throws -> Outcome {
        let info = await MainActor.run { DeviceInfo.current() }
        let idiom = await MainActor.run { UIDevice.current.userInterfaceIdiom }
        var metrics = Metrics()
        for key in ["model", "machine", "board", "system", "build"] { metrics.set(key, info[key] ?? "") }
        metrics.set("memory_mb", Int(ProcessInfo.processInfo.physicalMemory >> 20))
        return .judged(
            [
                ("UIDevice says iPad", info["model"] == "iPad" && idiom == .pad),
                ("hw.machine is an iPad identity", info["machine"]?.hasPrefix("iPad") == true),
                ("a build version", info["build"]?.isEmpty == false),
            ], metrics)
    }
}

struct PowerStateTest: HWTest {
    let id = "system.power"
    let name = "Battery, thermal state and low power mode"
    let subsystem = Subsystem.system

    static let batteryStates = ["unknown", "unplugged", "charging", "full"]

    func run(_ context: TestContext) async throws -> Outcome {
        let (level, state) = await MainActor.run { () -> (Float, UIDevice.BatteryState) in
            UIDevice.current.isBatteryMonitoringEnabled = true
            return (UIDevice.current.batteryLevel, UIDevice.current.batteryState)
        }
        let thermal = ProcessInfo.processInfo.thermalState
        var metrics = Metrics()
        metrics.set("battery_level", Double(level), digits: 2)
        metrics.set("battery_state", Self.batteryStates[state.rawValue])
        metrics.set("thermal", ["nominal", "fair", "serious", "critical"][thermal.rawValue])
        metrics.set("low_power", ProcessInfo.processInfo.isLowPowerModeEnabled)
        return .judged(
            [
                ("battery state known", state != .unknown),
                ("battery level in 0...1", level >= 0 && level <= 1),
                ("not thermally critical", thermal != .critical),
            ], metrics)
    }
}
