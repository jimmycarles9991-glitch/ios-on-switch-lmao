import Darwin
import Foundation
import HWTestCore
import HWTestKit
import UIKit

/// An icon render the way a helper asks for one: IconServices draws Settings' icon at the home screen's size in
/// iconservicesagent (ISIcon -prepareImageForDescriptor:, which crosses to the agent), timed cold and warm, with the
/// agent's phys_footprint, resident size and lifetime peak before and after (proc_pid_rusage), and once more when its
/// GPU's idle memory went back (the device's give-back, a second after its GPU idles), against its jetsam
/// InactiveHard limit (15 MB). A settled footprint above the limit fails: the agent would be killed as soon as it goes
/// inactive. The verdict reads the kernel's own comparison (JetsamExcess: the footprint past the inactive limit), which
/// the sandbox lets the app ask; the footprints themselves are reported where proc_pid_rusage is allowed. The agent's
/// own log line ("SwitchMetal: idle memory given back: footprint ...") names each term's owner.
struct IconRenderTest: HWTest {
    let id = "ui.icon_render"
    let name = "An icon render keeps iconservicesagent within its inactive memory limit"
    let subsystem = Subsystem.compositor

    static let agent = "iconservicesagent"
    static let inactiveHardMB = 15
    // GpuMemoryRelease.h's idle second and the give-back's own work after it
    static let giveBackNs: UInt64 = 2_500_000_000

    func run(_ context: TestContext) async throws -> Outcome {
        var metrics = Metrics()
        guard let icon = Self.icon("com.apple.Preferences"), let descriptor = Self.descriptor() else {
            return .skip("IconServices' ISIcon or ISImageDescriptor is missing")
        }
        let before = Self.footprint(Self.agent)
        let cold = Self.render(icon, descriptor)
        let warm = Self.render(icon, descriptor)
        let after = Self.footprint(Self.agent)
        try await Task.sleep(nanoseconds: Self.giveBackNs)
        let settled = Self.footprint(Self.agent)
        metrics.set("cold_ms", cold.ms, digits: 1)
        metrics.set("warm_ms", warm.ms, digits: 1)
        metrics.set("rendered", cold.rendered && warm.rendered)
        for (label, reading) in [("before", before), ("after", after), ("settled", settled)] {
            guard let reading else { continue }
            metrics.set("\(label)_footprint_mb", reading.footprintMB, digits: 1)
            metrics.set("\(label)_resident_mb", reading.residentMB, digits: 1)
            metrics.set("\(label)_peak_mb", reading.peakMB, digits: 1)
        }
        if !cold.rendered { return .fail("no image for Settings' icon", metrics) }
        guard let pid = RunningProcesses.pid(named: Self.agent, in: RunningProcesses.list(sysctl: "kern.proc.all"))
        else {
            return .fail("\(Self.agent) is not running after the render", metrics)
        }
        switch JetsamExcess.read(pid: pid) {
        case .failure(let error):
            metrics.set("memorystatus_errno", Int(error.code.rawValue))
            if let settled, settled.footprintMB > Double(Self.inactiveHardMB) {
                return .fail(Self.over(Int(settled.footprintMB), limit: Self.inactiveHardMB), metrics)
            }
            if settled != nil { return .pass(metrics) }
            return .skip(
                "\(Self.agent)'s footprint is unreadable: proc_pid_rusage refused and memorystatus_control "
                    + "errno \(error.code.rawValue)", metrics
            )
        case .success(let reading):
            metrics.set("inactive_limit_mb", reading.inactiveLimitMB)
            metrics.set("settled_excess_mb", reading.excessMB, digits: 1)
            if reading.excessBytes > 0 {
                return .fail(
                    Self.over(
                        reading.inactiveLimitMB + Int(reading.excessMB.rounded(.up)),
                        limit: reading.inactiveLimitMB
                    ),
                    metrics
                )
            }
            return .pass(metrics)
        }
    }

    static func over(_ mb: Int, limit: Int) -> String {
        "\(agent) holds \(mb) MB once its GPU memory went back after an icon render, "
            + "over its \(limit) MB inactive limit"
    }

    struct Reading {
        let footprintMB, residentMB, peakMB: Double
    }

    typealias Rusage = @convention(c) (Int32, Int32, UnsafeMutableRawPointer) -> Int32

    /// A process's footprint by name (the first pid with it): proc_pid_rusage's phys_footprint, resident size and lifetime
    /// peak footprint; nil when it is not running or its rusage is refused
    static func footprint(_ process: String) -> Reading? {
        guard let pid = RunningProcesses.pid(named: process, in: RunningProcesses.list(sysctl: "kern.proc.all")),
              let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "proc_pid_rusage")
        else { return nil }
        let rusage = unsafeBitCast(symbol, to: Rusage.self)
        var info = rusage_info_v4()
        guard rusage(pid, RUSAGE_INFO_V4, &info) == 0 else { return nil }
        let mb = { (v: UInt64) in Double(v) / 1_048_576 }
        return Reading(
            footprintMB: mb(info.ri_phys_footprint), residentMB: mb(info.ri_resident_size),
            peakMB: mb(info.ri_lifetime_max_phys_footprint)
        )
    }

    static func icon(_ bundle: String) -> NSObject? {
        guard let type = NSClassFromString("ISIcon") as? NSObject.Type else { return nil }
        let made = type.perform(NSSelectorFromString("alloc"))?.takeUnretainedValue() as? NSObject
        return made?.perform(NSSelectorFromString("initWithBundleIdentifier:"), with: bundle)?.takeUnretainedValue()
            as? NSObject
    }

    static func descriptor() -> NSObject? {
        guard let type = NSClassFromString("ISImageDescriptor") as? NSObject.Type else { return nil }
        let named = NSSelectorFromString("imageDescriptorNamed:")
        return type.perform(named, with: "com.apple.IconServices.ImageDescriptor.HomeScreen")?.takeUnretainedValue()
            as? NSObject
    }

    static func render(_ icon: NSObject, _ descriptor: NSObject) -> (ms: Double, rendered: Bool) {
        let start = CFAbsoluteTimeGetCurrent()
        let image = icon.perform(NSSelectorFromString("prepareImageForDescriptor:"), with: descriptor)?
            .takeUnretainedValue()
        return ((CFAbsoluteTimeGetCurrent() - start) * 1000, image != nil)
    }
}
