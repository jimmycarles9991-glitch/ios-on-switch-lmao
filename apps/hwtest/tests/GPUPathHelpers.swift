import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// The capability flags the GM20B kernel advertises and how often this process took each path behind them (debug
/// kernels, drivers/TegraGM20B/SwitchGpuPathStats.h): asked on the test's own TegraGM20B connection
/// (TegraGM20BPathsABI.h), which the app's sandbox admits where it denies the kern.switch_gpu_paths sysctl.
public struct GPUPaths {
    public let flags: UInt32
    public let counts: [String: Int]

    public static let method: UInt32 = 0x5050
    public static let foreignWaitsOp: UInt32 = 6
    public static let zbcOp: UInt32 = 9

    public static let replySize = 1024

    /// The GPU's ZBC colour table (TegraGM20BPathsOpZbc); nil from a kernel that does not answer it.
    public static func zbcTable(_ connection: GPUConnection?) -> ZbcTable? {
        guard let connection else { return nil }
        let (kr, reply) = connection.call(method, GPUConnection.words([zbcOp, 0]), outputSize: replySize)
        guard kr == KERN_SUCCESS else { return nil }
        return ZbcTable.parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self))
    }

    public static func foreignWaits(_ connection: GPUConnection?) -> [String: Int]? {
        guard let connection else { return nil }
        let (kr, reply) = connection.call(method, GPUConnection.words([foreignWaitsOp, 0]), outputSize: replySize)
        guard kr == KERN_SUCCESS else { return nil }
        return parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)).counts
    }

    public enum Reading {
        case counts(GPUPaths)
        /// Neither route answered: the connection's (no_connection, release_kernel or a kern_return_t) and the
        /// sysctl's errno.
        case unreadable(connection: String, sysctl: Int32)
    }

    public static func infoFlags(_ connection: GPUConnection?) -> UInt32? {
        guard let connection else { return nil }
        let (kr, reply) = connection.call(
            GPUConnection.FencePage.getInfo, [], outputSize: GPUConnection.FencePage.infoBytes
        )
        guard kr == KERN_SUCCESS, reply.count >= 8 else { return nil }
        return GPUConnection.uint32(reply, 4)
    }

    public static func read(_ connection: GPUConnection?) -> Reading {
        var asked = "no_connection"
        if let connection {
            let (kr, reply) = connection.call(method, GPUConnection.words([0, 0]), outputSize: replySize)
            if kr == KERN_SUCCESS { return .counts(parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self))) }
            asked = kr == GPUConnection.kIOReturnUnsupported ? "release_kernel" : GPUConnection.hex(kr)
        }
        switch KernelSysctl.string("kern.switch_gpu_paths") {
        case .value(let line): return .counts(parse(line))
        case .failed(let error): return .unreadable(connection: asked, sysctl: error)
        }
    }

    public static func parse(_ line: String) -> GPUPaths {
        var flags: UInt32 = 0
        var counts: [String: Int] = [:]
        for field in line.split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1).map(String.init)
            guard pair.count == 2 else { continue }
            if pair[0] == "flags" {
                flags = UInt32(pair[1].dropFirst(2), radix: 16) ?? 0
            } else if let count = Int(pair[1]) {
                counts[pair[0]] = count
            }
        }
        return GPUPaths(flags: flags, counts: counts)
    }

    public func taken(_ name: String, since before: GPUPaths) -> Int {
        (counts[name] ?? 0) - (before.counts[name] ?? 0)
    }
}

/// The parts of a GPUPathTest's workload that could not run here, each with why and the counters only it would have
/// taken: exercise() reports them through `current`, and run() leaves those counters out of its "no X while it ran".
/// A part that could not run because of the driver's own failing fails the test.
public final class PathParts: @unchecked Sendable {
    @TaskLocal public static var current: PathParts?
    public private(set) var skipped: [(why: String, counters: [String])] = []
    public private(set) var failed: [String] = []

    public init() {}

    public func skip(_ why: String, counters: [String]) { skipped.append((why, counters)) }
    public func fail(_ why: String, counters: [String]) {
        skipped.append((why, counters))
        failed.append(why)
    }

    /// The route HW Test's own command buffers take, from a blit fill through the test's queue that the path tally
    /// leaves out: a promise test whose maker or reader is this process's own Metal work runs its part only on "gm20b".
    public static func ownRoute(_ gpu: GPU) -> String {
        guard let buffer = gpu.device.makeBuffer(length: 256, options: .storageModeShared),
              let commands = gpu.queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder()
        else { return "none" }
        blit.fill(buffer: buffer, range: 0 ..< 256, value: 0x5a)
        blit.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        return GM20BFacade.path(of: commands)
    }

    /// Whether this process's own command buffers take the driver; when not, the part does not run and the test fails
    /// with the route.
    public static func ownRouteRaw(_ gpu: GPU, part: String, counters: [String], _ metrics: inout Metrics) -> Bool {
        let route = ownRoute(gpu)
        metrics.set("own_route", route)
        if route == "gm20b" { return true }
        current?.fail("\(part): HW Test's own command buffers go \(route)", counters: counters)
        return false
    }
}

/// One GM20B kernel path behind a capability bit, on the facade: the kernel advertises the bit, a workload that
/// needs the path gives the right bytes with every command buffer on the driver, and the kernel counted the path for
/// this process while it ran.
public protocol GPUPathTest: HWTest {
    /// The capability bit that still gates the path, if any
    var bit: Int? { get }
    var counters: [String] { get }
    /// The workload's failures, none when it gave the right bytes.
    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String]
}

extension GPUPathTest {
    public var subsystem: Subsystem { .metal }
    public var bit: Int? { nil }

    public func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let connection = GPUConnection()
        var metrics = Metrics()
        let before: GPUPaths?
        let flags: UInt32
        switch GPUPaths.read(connection) {
        case .counts(let paths):
            before = paths
            flags = paths.flags != 0 ? paths.flags : (GPUPaths.infoFlags(connection) ?? 0)
        case .unreadable(let asked, let error):
            if counters.isEmpty && asked == "release_kernel" && error == ENOENT {
                before = nil
                flags = GPUPaths.infoFlags(connection) ?? 0
            } else {
                metrics.set("connection", asked)
                metrics.set("sysctl", KernelSysctl.code(error))
                if asked == "release_kernel" && error == ENOENT {
                    let why = KernelSysctl.isReleaseKernel()
                        ? "no path counters: a release kernel"
                        : "no path counters (build with --//bazel:stats_gpu)"
                    return .skip(why, metrics)
                }
                let why = "the path counters are unreadable: connection \(asked), sysctl \(KernelSysctl.code(error))"
                return .fail(why, metrics)
            }
        }
        metrics.set("flags", String(flags, radix: 16))
        var failures: [String] = []
        if let bit, flags & (1 << UInt32(bit)) == 0 {
            failures.append("the kernel does not advertise bit \(bit)")
        }
        let parts = PathParts()
        failures += try await PathParts.$current.withValue(parts) { try await exercise(gpu, context, &metrics) }
        failures += parts.failed
        if let before {
            let after: GPUPaths
            if case .counts(let paths) = GPUPaths.read(connection) { after = paths } else { after = before }
            let skippedCounters = Set(parts.skipped.flatMap(\.counters))
            for counter in counters {
                let taken = after.taken(counter, since: before)
                metrics.set(counter, taken)
                if taken <= 0 && !skippedCounters.contains(counter) { failures.append("no \(counter) while it ran") }
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        if gpu.paths.paths.keys.contains(where: { $0 != "gm20b" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        if !failures.isEmpty { return .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof) }
        if !parts.skipped.isEmpty { return .skip(parts.skipped.map(\.why).joined(separator: "; "), metrics) }
        return .pass(metrics, proof: gpu.proof)
    }

    public static func wrong(_ got: [UInt8], _ want: [UInt8]) -> Int {
        zip(got, want).filter { $0 != $1 }.count + abs(got.count - want.count)
    }
}
