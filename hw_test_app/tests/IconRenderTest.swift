import Darwin
import Foundation
import HWTestCore
import UIKit

/// An icon render the way a helper asks for one: IconServices draws Settings' icon at the home screen's size in
/// iconservicesagent (ISIcon -prepareImageForDescriptor:, which crosses to the agent), timed cold and warm, with the
/// agent's phys_footprint, resident size and lifetime peak before and after (proc_pid_rusage), and once more when its
/// GPU's idle memory went back (the raw device's give-back, a second after its GPU idles), against its jetsam
/// InactiveHard limit of 15 MB. A settled footprint above the limit fails: the agent would be killed as soon as it goes
/// inactive. The agent's own log line ("raw: idle memory given back: footprint ...") names each term's owner.
struct IconRenderTest: HWTest {
    let id = "ui.icon_render"
    let name = "An icon render keeps iconservicesagent within its inactive memory limit"
    let subsystem = Subsystem.compositor

    static let agent = "iconservicesagent"
    static let inactiveHardMB = 15
    // NMB_IDLE_RELEASE_MS outside the render server (TegraGM20BMetalPlugin.mm) and the give-back's own work after it
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
        guard let settled else { return .skip("\(Self.agent)'s rusage is unreadable from this app", metrics) }
        if !cold.rendered { return .fail("no image for Settings' icon", metrics) }
        if settled.footprintMB > Double(Self.inactiveHardMB) {
            return .fail(
                "\(Self.agent) holds \(Int(settled.footprintMB)) MB once its GPU memory went back after an icon render, "
                    + "over its \(Self.inactiveHardMB) MB inactive limit", metrics)
        }
        return .pass(metrics)
    }

    struct Reading {
        let footprintMB, residentMB, peakMB: Double
    }

    typealias Rusage = @convention(c) (Int32, Int32, UnsafeMutableRawPointer) -> Int32

    /// A process's footprint by name (the first pid with it): proc_pid_rusage's phys_footprint, resident size and lifetime
    /// peak footprint; nil when it is not running or its rusage is refused
    static func footprint(_ process: String) -> Reading? {
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
        var size = 0
        guard sysctl(&mib, 4, nil, &size, nil, 0) == 0, size > 0 else { return nil }
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: size / MemoryLayout<kinfo_proc>.stride + 8)
        size = procs.count * MemoryLayout<kinfo_proc>.stride
        guard sysctl(&mib, 4, &procs, &size, nil, 0) == 0,
            let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "proc_pid_rusage")
        else { return nil }
        let rusage = unsafeBitCast(symbol, to: Rusage.self)
        for proc in procs.prefix(size / MemoryLayout<kinfo_proc>.stride) {
            var comm = proc.kp_proc.p_comm
            let name = withUnsafeBytes(of: &comm) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
            guard name == process else { continue }
            var info = rusage_info_v4()
            guard rusage(proc.kp_proc.p_pid, RUSAGE_INFO_V4, &info) == 0 else { return nil }
            let mb = { (v: UInt64) in Double(v) / 1_048_576 }
            return Reading(
                footprintMB: mb(info.ri_phys_footprint), residentMB: mb(info.ri_resident_size),
                peakMB: mb(info.ri_lifetime_max_phys_footprint))
        }
        return nil
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
