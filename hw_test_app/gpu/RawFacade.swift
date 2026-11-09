import Foundation
import Metal

/// What the Switch's Metal stack tells a process about its GPU paths (drivers/TegraGM20BMetal/raw/runtime/RawMetal.mm,
/// debug builds): the default device is the raw GM20B facade unless the process runs with TEGRA_RAW_METAL=0 or the
/// pack was refused; the facade hands out the m2v device it wraps, counts its command buffers per path and names each
/// one's path. On a Mac (the host tests) or a device without them, every answer says so instead.
public enum RawFacade {
    static let facadeClass = "RawGM20BMetalDevice"

    /// "raw" (the facade), "m2v" (the Metal-over-Vulkan device itself) or "apple" (a Mac's GPU).
    public static func kind(of device: MTLDevice) -> String {
        if NSStringFromClass(type(of: device as AnyObject)) == facadeClass { return "raw" }
        #if os(macOS)
            return "apple"
        #else
            return "m2v"
        #endif
    }

    public static func m2vDevice(of device: MTLDevice) -> MTLDevice? {
        ask(device, "switchM2VDevice") as? MTLDevice
    }

    /// The facade's cumulative command buffer counters (empty off the facade).
    public static func counters(of device: MTLDevice) -> [String: Int] {
        guard let counters = ask(device, "switchRawCounters") as? [String: NSNumber] else { return [:] }
        return counters.mapValues(\.intValue)
    }

    /// The bytes the facade's raw device holds now (empty off the facade): "pool" (its pool, block-linear and depth
    /// chunks), "wc" (write-combined chunks) and "heap" (the shader heap's bound parts).
    public static func memory(of device: MTLDevice) -> [String: Int] {
        guard let memory = ask(device, "switchRawMemory") as? [String: NSNumber] else { return [:] }
        return memory.mapValues(\.intValue)
    }

    /// The facade's compressed memory (empty off the facade): "held", "held_bytes" and "views" now, "made" and "refused"
    /// since the device opened.
    public static func compression(of device: MTLDevice) -> [String: Int] {
        guard let compression = ask(device, "switchRawCompression") as? [String: NSNumber] else { return [:] }
        return compression.mapValues(\.intValue)
    }

    /// "raw", "m2v:<reason>:<predicate>" or "split:..." on the facade, "device" anywhere else.
    public static func path(of buffer: MTLCommandBuffer) -> String {
        ask(buffer, "switchRawPath") as? String ?? "device"
    }

    static func ask(_ object: Any, _ name: String) -> Any? {
        let selector = NSSelectorFromString(name)
        guard let object = object as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector)?.takeUnretainedValue()
    }
}
