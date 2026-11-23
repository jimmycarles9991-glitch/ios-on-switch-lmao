import Foundation
import HWTestCore
import Metal

/// What the Switch's Metal stack tells a process about its GPU paths (drivers/TegraGM20BMetal/TegraGM20BMetal.mm,
/// debug builds): the default device is the raw GM20B facade (a process it cannot serve gets no Metal device); the
/// facade counts its command buffers and the ones that failed, and names each one's path. On a Mac (the host tests),
/// every answer says so instead.
public enum GM20BFacade {
    static let facadeClass = "GM20BGM20BMetalDevice"

    /// "gm20b" (the facade) or "apple" (Apple's GPU: a Mac's).
    public static func kind(of device: MTLDevice) -> String {
        NSStringFromClass(type(of: device as AnyObject)) == facadeClass ? "gm20b" : "apple"
    }

    /// Whether the facade drew `state` from the pack's record rather than one made at its creation (nil off the
    /// facade or in a release runtime, TegraGM20BMetalObjectProbe.mm).
    public static func packed(_ state: MTLRenderPipelineState) -> Bool? {
        ((ask(state, "switchMetalObject") as? [String: Any])?["packed"] as? NSNumber)?.boolValue
    }

    /// The set-0 entries of the record the facade draws `state` from (0: none; nil off the facade or in a release
    /// runtime).
    public static func entries(of state: MTLRenderPipelineState) -> Int? {
        ((ask(state, "switchMetalObject") as? [String: Any])?["entries"] as? NSNumber)?.intValue
    }

    /// The facade's cumulative command buffer counters (empty off the facade): "gm20b", "failed", "failed.<predicate>",
    /// "skipped", "skipped.<predicate>" (FailedCounts) and the driver's own.
    public static func counters(of device: MTLDevice) -> [String: Int] {
        guard let counters = ask(device, "switchMetalCounters") as? [String: NSNumber] else { return [:] }
        return counters.mapValues(\.intValue)
    }

    /// Why a test that reads `dependency` (a counter key or a selector the facade answers in debug builds only) skips
    /// on `device`'s facade, which lacks it: a release runtime answers no counters at all; a debug one that lacks it
    /// has dropped what the test reads, and the reason names it (tools/devicerun hwtest.missing-key fails on that)
    public static func missing(_ dependency: String, on device: MTLDevice) -> String {
        counters(of: device).isEmpty
            ? "no \(dependency): a release runtime" : "missing key \(dependency) in a debug runtime"
    }

    /// The latest resource set registration the kernel refused: its member and the reason (debug builds; "-" for none
    /// or off the facade).
    public static func setRefusal(of device: MTLDevice) -> String {
        let refusal = ask(device, "switchMetalSetRefusal") as? String ?? ""
        return refusal.isEmpty ? "-" : refusal
    }

    /// The facade's encode profile since the device opened (debug builds; empty off the facade or in a release
    /// runtime): "<part>_us", "<part>_self_us" and "<part>_calls" for each part of encode_profile.h, its events
    /// ("mallocs", "locks", "sends", "applied", ...), "encodes", "malloc_encodes" and "wall_us".
    public static func encodeProfile(of device: MTLDevice) -> [String: Double] {
        guard let profile = ask(device, "switchMetalEncodeProfile") as? [String: NSNumber] else { return [:] }
        return profile.mapValues(\.doubleValue)
    }

    /// The bytes the facade's raw device holds now (empty off the facade): "pool" (its pool, block-linear and depth
    /// chunks), "wc" (write-combined chunks), "heap" (the shader heap's bound parts) and "headers_left" (texture
    /// headers not held).
    public static func memory(of device: MTLDevice) -> [String: Int] {
        guard let memory = ask(device, "switchMetalMemory") as? [String: NSNumber] else { return [:] }
        return memory.mapValues(\.intValue)
    }

    /// The facade's compressed memory (empty off the facade): "held", "held_bytes" and "views" now, "made" and
    /// "refused" since the device opened.
    public static func compression(of device: MTLDevice) -> [String: Int] {
        guard let compression = ask(device, "switchMetalCompression") as? [String: NSNumber] else { return [:] }
        return compression.mapValues(\.intValue)
    }

    /// How the fetching draws between two counter reads kept raster order (fetch_order.h FetchCounts):
    /// "interlocked:1,record.restart:2" and so on, "-" where no fetching draw was counted (or off the facade).
    public static func fetchRoutes(before: [String: Int], after: [String: Int]) -> String {
        let changed = after.filter { $0.key.hasPrefix("fetch_") }
            .map { ($0.key, $0.value - (before[$0.key] ?? 0)) }
            .filter { $0.1 != 0 }
            .map { "\($0.0.dropFirst("fetch_".count)):\($0.1)" }
        return changed.isEmpty ? "-" : changed.sorted().joined(separator: ",")
    }

    /// "gm20b", "error:<predicate>" or "error:none" (a failed one) on the facade, "device" anywhere else.
    public static func path(of buffer: MTLCommandBuffer) -> String {
        ask(buffer, "switchMetalPath") as? String ?? "device"
    }

    /// The clock of the buffer's GPUStartTime/GPUEndTime on the facade (debug builds): "ns", its GPU stamps, or "host",
    /// the host's window from submission to completion standing in for them; nil elsewhere.
    public static func gpuClock(of buffer: MTLCommandBuffer) -> String? {
        ask(buffer, "switchGpuClock") as? String
    }

    /// Whether a render state's record came out of the pack at its creation (nil off the facade or in a release
    /// build).
    public static func packHit(_ state: MTLRenderPipelineState) -> Bool? {
        (ask(state, "switchPackHit") as? NSNumber)?.boolValue
    }

    public static func wholePrograms(in buffer: MTLCommandBuffer) -> Bool {
        (ask(buffer, "switchMetalWholePrograms") as? NSNumber)?.boolValue ?? false
    }

    /// How the facade got a render state's record (debug builds; nil elsewhere): "how" (pack, table, store, compiled,
    /// joined, stored, none), "wait_us" and "qos" (the creating thread's wait for the shader compiler and its
    /// class), "ran_class" (the compiler's band for it, 4 user-interactive).
    public static func compile(of state: MTLRenderPipelineState) -> [String: Any]? {
        ask(state, "switchMetalCompile") as? [String: Any]
    }

    /// The shader compiler's store the facade maps (debug builds): its stack key and pack rules' digest in hex, "" for
    /// none; nil off the facade.
    public static func compilerStore(of device: MTLDevice) -> String? {
        ask(device, "switchCompilerStore") as? String
    }

    /// What the submit worker encoded of a completed buffer ahead of its commit (debug builds; empty elsewhere):
    /// "encoders", "pipelined", "abandons", "veto" and "given_back" (SwitchMetalPipelineVeto bits: the vetoes recorded, and the
    /// reasons its encodes ahead were given back) and "commit_to_gpu_start_us".
    public static func pipelined(_ buffer: MTLCommandBuffer) -> [String: NSNumber] {
        ask(buffer, "switchMetalPipelined") as? [String: NSNumber] ?? [:]
    }

    /// The buffer binds every state as the inline copy it was before state segments (debug builds, before its commit).
    public static func inlineStates(in buffer: MTLCommandBuffer) -> Bool {
        (ask(buffer, "switchMetalInlineStates") as? NSNumber)?.boolValue ?? false
    }

    static func ask(_ object: Any, _ name: String) -> Any? {
        let selector = NSSelectorFromString(name)
        guard let object = object as? NSObject, object.responds(to: selector) else { return nil }
        return object.perform(selector)?.takeUnretainedValue()
    }
}

extension Outcome {
    /// On the facade, FAIL naming each command buffer of the process that failed since `gpu` was made, a framework's
    /// included, but the `expected` ones, and whose it was: one of `gpu`'s own or one the test did not commit
    /// (FailedAttribution.check); with `framework` (work MPS or Core Image records), also each call skipped meanwhile
    /// (FailedCounts.skipCheck). Ahead of whatever else the test found; unchanged off the facade. paths=, failed_cbs=,
    /// failed_why= and the cause fields on the line.
    public func onRawPath(_ gpu: GPU, expecting expected: [String: Int] = [:], framework: Bool = false) -> Outcome {
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else { return self }
        var marked = self
        let own = gpu.paths
        marked.metrics.set("paths", own.isEmpty ? "-" : own.text)
        let after = GM20BFacade.counters(of: gpu.device)
        guard !after.isEmpty else { return marked.failing(offRaw: own.offRaw) }
        let failed = FailedCounts.judged(
            before: gpu.startCounters, after: after, expecting: expected, own: own, &marked.metrics
        )
        marked = marked.failing(unless: failed)
        guard framework else { return marked }
        return marked.failing(unless: FailedCounts(before: gpu.startCounters, after: after).skipCheck())
    }
}

/// The facade's paths of command buffers a test makes itself rather than through a GPU (a layer's drawables), each read
/// once it completed.
public final class CommandPaths: @unchecked Sendable {
    private let lock = NSLock()
    private var tally = PathTally()

    public init() {}

    public func watch(_ buffer: MTLCommandBuffer) {
        buffer.addCompletedHandler { [self] done in
            let path = GM20BFacade.path(of: done)
            lock.lock()
            tally.add(path)
            lock.unlock()
        }
    }

    public var paths: PathTally {
        lock.lock()
        defer { lock.unlock() }
        return tally
    }
}

extension Outcome {
    /// onRawPath for command buffers `watched` saw on `device`.
    public func onRawPath(_ watched: CommandPaths, device: MTLDevice?) -> Outcome {
        guard let device, GM20BFacade.kind(of: device) == "gm20b" else { return self }
        var marked = self
        let paths = watched.paths
        marked.metrics.set("paths", paths.isEmpty ? "-" : paths.text)
        return marked.failing(offRaw: paths.offRaw)
    }
}
