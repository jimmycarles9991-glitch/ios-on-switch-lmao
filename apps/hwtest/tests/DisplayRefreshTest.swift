import CoreFoundation
import Darwin
import HWTestCore
import UIKit

/// IOMFBDisplayRefresh, which QuartzCore's AppleDisplay::update_display_limits reads (24B119) and
/// IOMobileFramebufferLegacy::setDisplayRefreshProperties publishes. Its shortest refresh interval is the panel's period
/// at its highest rate, and on a fixed-rate internal panel the longest is the same (the family answers 1/24 s for an
/// external display); *_mach fields are mach ticks, period_mach the period expected. The app's sandbox denies it
/// IOMobileFramebufferCopyProperty's read of the property (iokit-get-properties on AppleCLCD), so the values come from
/// the display's copy, kern.switch_display_refresh_* (debug kernels; source= says which was read).
struct DisplayRefreshTest: HWTest {
    let id = "display.refresh_dictionary"
    let name = "The display publishes IOMFBDisplayRefresh"
    let subsystem = Subsystem.compositor

    static let keys = [
        "displayMinRefreshInterval", "displayMinRefreshIntervalMachTime", "displayMaxRefreshInterval",
        "displayMaxRefreshIntervalMachTime", "displayRefreshStep", "displayRefreshStepMachTime",
    ]
    /// Each key's field, and the display's kern.switch_display_refresh_<field>.
    static let counters = ["min", "min_mach", "max", "max_mach", "step", "step_mach"]

    func run(_ context: TestContext) async throws -> Outcome {
        let fps = await MainActor.run { UIScreen.main.maximumFramesPerSecond }
        let period = Self.machTicks(seconds: 1 / Double(max(fps, 1)))
        let display = Self.dictionary()
        let found = display.values
        var metrics = Metrics()
        metrics.set("display_found", display.found)
        metrics.set("open_kr", display.status.map { String(UInt32(bitPattern: $0), radix: 16) } ?? "not_opened")
        metrics.set("present", found != nil)
        metrics.set("period_mach", Int(period))
        var values: [String: Int64] = [:]
        var unread: Int32 = 0
        for (key, field) in zip(Self.keys, Self.counters) {
            if let number = found?[key] as? NSNumber {
                values[field] = number.int64Value
                continue
            }
            switch KernelSysctl.integer("kern.switch_display_refresh_" + field, as: Int64.self) {
            case .value(let value) where value >= 0: values[field] = value
            case .value: break
            case .failed(let error): unread = error
            }
        }
        if found == nil, values.isEmpty, unread != 0 {
            return .skip(KernelSysctl.unreadable("kern.switch_display_refresh_*", unread), metrics)
        }
        metrics.set("source", found != nil ? "property" : "sysctl")
        for field in Self.counters {
            if let value = values[field] { metrics.set(field, Int(value)) }
        }
        let minMach = Double(values["min_mach"] ?? 0)
        let internalPanel = values["max_mach"].map { $0 == values["min_mach"] } ?? false
        return .judged(
            [
                ("the display published IOMFBDisplayRefresh", !values.isEmpty),
                ("all six keys", values.count == Self.keys.count),
                ("min_mach within 1% of period_mach", abs(minMach - period) <= period / 100),
                ("max_mach == min_mach (an internal panel)", internalPanel),
            ], metrics
        )
    }

    static func machTicks(seconds: Double) -> Double {
        var base = mach_timebase_info_data_t()
        mach_timebase_info(&base)
        return seconds * 1e9 * Double(base.denom) / Double(base.numer)
    }

    typealias MainDisplay = @convention(c) (UnsafeMutablePointer<Unmanaged<CFTypeRef>?>) -> kern_return_t
    typealias CopyProperty = @convention(c) (CFTypeRef, CFString) -> Unmanaged<CFTypeRef>?

    static func dictionary() -> (found: Bool, status: kern_return_t?, values: [String: Any]?) {
        guard
            let library = dlopen(
                "/System/Library/PrivateFrameworks/IOMobileFramebuffer.framework/IOMobileFramebuffer", RTLD_NOW
            )
        else { return (false, nil, nil) }
        defer { dlclose(library) }
        guard let mainDisplay = dlsym(library, "IOMobileFramebufferGetMainDisplay"),
              let copyProperty = dlsym(library, "IOMobileFramebufferCopyProperty")
        else { return (false, nil, nil) }
        var display: Unmanaged<CFTypeRef>?
        let status = unsafeBitCast(mainDisplay, to: MainDisplay.self)(&display)
        defer { display?.release() }
        guard status == KERN_SUCCESS, let display else { return (false, status, nil) }
        let value = unsafeBitCast(copyProperty, to: CopyProperty.self)(
            display.takeUnretainedValue(), "IOMFBDisplayRefresh" as CFString
        )
        return (true, status, value?.takeRetainedValue() as? [String: Any])
    }
}
