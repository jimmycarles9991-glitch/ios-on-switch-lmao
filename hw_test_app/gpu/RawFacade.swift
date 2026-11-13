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

    /// Whether the facade still wraps an m2v device (a debug build that has one answers switchM2VDevice), asked
    /// without opening it.
    public static func wrapsM2V(_ device: MTLDevice) -> Bool {
        (device as AnyObject).responds(to: NSSelectorFromString("switchM2VDevice"))
    }

    /// Whether the facade drew `state` from the raw pack's record rather than one made at its creation (nil off the
    /// facade or in a release runtime, raw/runtime/RawMetalObjectProbe.mm).
    public static func packed(_ state: MTLRenderPipelineState) -> Bool? {
        ((ask(state, "switchRawObject") as? [String: Any])?["packed"] as? NSNumber)?.boolValue
    }

    /// The facade's record of its m2v device, read without opening it (debug builds; empty off the facade):
    /// "opened", "opens", "opened_for" and "needs" (each need the process asked it for, with its count)
    public static func m2vRecord(of device: MTLDevice) -> [String: Any] {
        ask(device, "switchRawM2V") as? [String: Any] ?? [:]
    }

    /// The facade's cumulative command buffer counters (empty off the facade).
    public static func counters(of device: MTLDevice) -> [String: Int] {
        guard let counters = ask(device, "switchRawCounters") as? [String: NSNumber] else { return [:] }
        return counters.mapValues(\.intValue)
    }

    /// The latest resource set registration the kernel refused: its member and the reason (debug builds; "-" for none
    /// or off the facade).
    public static func setRefusal(of device: MTLDevice) -> String {
        let refusal = ask(device, "switchRawSetRefusal") as? String ?? ""
        return refusal.isEmpty ? "-" : refusal
    }

    /// The facade's encode profile since the device opened (debug builds; empty off the facade or in a release
    /// runtime): "<part>_us", "<part>_self_us" and "<part>_calls" for each part of raw_encode_profile.h, its events
    /// ("mallocs", "locks", "sends", "applied", ...), "encodes", "malloc_encodes" and "wall_us".
    public static func encodeProfile(of device: MTLDevice) -> [String: Double] {
        guard let profile = ask(device, "switchRawEncodeProfile") as? [String: NSNumber] else { return [:] }
        return profile.mapValues(\.doubleValue)
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

    /// How the fetching draws between two counter reads kept raster order (raw_fetch_order.h FetchCounts):
    /// "interlocked:1,record.restart:2" and so on, "-" where no fetching draw was counted (or off the facade).
    public static func fetchRoutes(before: [String: Int], after: [String: Int]) -> String {
        let changed = after.filter { $0.key.hasPrefix("fetch_") }
            .map { ($0.key, $0.value - (before[$0.key] ?? 0)) }
            .filter { $0.1 != 0 }
            .map { "\($0.0.dropFirst("fetch_".count)):\($0.1)" }
        return changed.isEmpty ? "-" : changed.sorted().joined(separator: ",")
    }

    /// "raw", "m2v:<reason>:<predicate>" or "split:..." on the facade, "device" anywhere else.
    public static func path(of buffer: MTLCommandBuffer) -> String {
        ask(buffer, "switchRawPath") as? String ?? "device"
    }

    /// The clock of the buffer's GPUStartTime/GPUEndTime on the facade (debug builds): "ns", its GPU stamps, or "host",
    /// the host's window from submission to completion standing in for them; nil elsewhere.
    public static func gpuClock(of buffer: MTLCommandBuffer) -> String? {
        ask(buffer, "switchGpuClock") as? String
    }

    /// Whether a render state's record came out of the raw pack at its creation (nil off the facade or in a release build).
    public static func packHit(_ state: MTLRenderPipelineState) -> Bool? {
        (ask(state, "switchPackHit") as? NSNumber)?.boolValue
    }

    public static func wholePrograms(in buffer: MTLCommandBuffer) -> Bool {
        (ask(buffer, "switchRawWholePrograms") as? NSNumber)?.boolValue ?? false
    }

    /// How the facade got a render state's record (debug builds; nil elsewhere): "how" (pack, table, store, compiled,
    /// joined, stored, here, none), "wait_us" and "qos" (the creating thread's wait for the shader compiler and its
    /// class), "ran_class" (the compiler's band for it, 4 user-interactive).
    public static func compile(of state: MTLRenderPipelineState) -> [String: Any]? {
        ask(state, "switchRawCompile") as? [String: Any]
    }

    /// The shader compiler's store the facade maps (debug builds): its stack key and pack rules' digest in hex, "" for
    /// none; nil off the facade.
    public static func compilerStore(of device: MTLDevice) -> String? {
        ask(device, "switchCompilerStore") as? String
    }

    /// The buffer binds every state as the inline copy it was before state segments (debug builds, before its commit).
    public static func inlineStates(in buffer: MTLCommandBuffer) -> Bool {
        (ask(buffer, "switchRawInlineStates") as? NSNumber)?.boolValue ?? false
    }

    static func ask(_ object: Any, _ name: String) -> Any? {
        let selector = NSSelectorFromString(name)
        guard let object = object as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector)?.takeUnretainedValue()
    }
}
