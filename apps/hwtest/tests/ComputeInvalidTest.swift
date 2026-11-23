import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Compute work Metal's validation would stop, on the driver. A dispatch with no compute state set fails its command
/// buffer with MTLCommandBufferErrorInvalidResource and the facade's compute predicate (no_state_error=, no_state_path=).
/// An indirect dispatch whose argument buffer holds 8 of the 12 bytes ends as Apple's GPU ends it (short_ending=,
/// short_path=; Self.short is what it is held to). The kernel writes only inside its buffer. The stateless one is the
/// only command buffer that fails (failed_cbs=, failed_why=).
struct ComputeInvalidTest: HWTest {
    let id = "gpu.gm20b.compute_invalid"
    let name =
        "A dispatch with no compute state fails InvalidResource and a short indirect one ends as Metal's"
    let subsystem = Subsystem.metal

    enum Ending: Equatable {
        case completes
        case fails(MTLCommandBufferError.Code)
    }

    /// An indirect dispatch whose arguments are 8 bytes long, as an Apple GPU ends it
    static let short: (ending: Ending, path: String) = (.completes, "gm20b")

    static let capacity = 1024

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    kernel void invalid_marks(device uint *out [[buffer(0)]], constant uint &capacity [[buffer(1)]],
                              uint i [[thread_position_in_grid]]) {
        if (i < capacity) out[i] = i + 1;
    }
    """

    static func ending(_ buffer: MTLCommandBuffer) -> Ending {
        guard buffer.status == .error else { return .completes }
        let code = (buffer.error as NSError?)?.code ?? 0
        return .fails(MTLCommandBufferError.Code(rawValue: UInt(code)) ?? .internal)
    }

    static func shown(_ ending: Ending) -> String {
        switch ending {
        case .completes: return "completes"
        case .fails(let code): return "error_\(code.rawValue)"
        }
    }

    func dispatch(_ gpu: GPU, _ encode: @escaping (MTLComputeCommandEncoder) -> Void) throws -> MTLCommandBuffer {
        let buffer = try gpu.commit { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encode(encoder)
            encoder.endEncoding()
        }
        buffer.waitUntilCompleted()
        return buffer
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        guard let function = library.makeFunction(name: "invalid_marks") else { throw TestFailure("no kernel") }
        let state = try await gpu.device.makeComputePipelineState(function: function)
        let out = try gpu.buffer(Self.capacity * 4)
        var capacity = UInt32(Self.capacity)
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []

        let stateless = try dispatch(gpu) { encoder in
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.setBytes(&capacity, length: 4, index: 1)
            encoder.dispatchThreadgroups(
                MTLSize(width: 4, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
        }
        let statelessPath = GM20BFacade.path(of: stateless)
        metrics.set("no_state_error", Self.shown(Self.ending(stateless)))
        metrics.set("no_state_path", statelessPath)
        checks.append(
            ("no state: MTLCommandBufferErrorInvalidResource", Self.ending(stateless) == .fails(.invalidResource))
        )
        checks.append(("no state: path error:compute", statelessPath == "error:compute"))

        guard let arguments = gpu.device.makeBuffer(length: 8, options: .storageModeShared) else {
            throw TestFailure("no 8-byte buffer")
        }
        arguments.contents().storeBytes(of: SIMD2<UInt32>(2, 1), as: SIMD2<UInt32>.self)
        memset(out.contents(), 0, Self.capacity * 4)
        let short = try dispatch(gpu) { encoder in
            encoder.setComputePipelineState(state)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.setBytes(&capacity, length: 4, index: 1)
            encoder.dispatchThreadgroups(
                indirectBuffer: arguments, indirectBufferOffset: 0,
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
        }
        let shortPath = GM20BFacade.path(of: short)
        let written = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: UInt32.self, capacity: Self.capacity), count: Self.capacity
        ).filter { $0 != 0 }.count
        metrics.set("short_ending", Self.shown(Self.ending(short)))
        metrics.set("short_path", shortPath)
        metrics.set("short_written", written)
        checks.append(
            ("short arguments: ends \(Self.shown(Self.short.ending))", Self.ending(short) == Self.short.ending)
        )
        checks.append(("short arguments: path \(Self.short.path)", shortPath == Self.short.path))

        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(
            FailedCounts.judged(before: before, after: counted, expecting: ["compute": 1], own: gpu.paths, &metrics)
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
