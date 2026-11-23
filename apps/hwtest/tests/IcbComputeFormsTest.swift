import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Compute dispatches issued through an indirect command buffer (one concurrent dispatch, executed once by a compute
/// encoder in a command buffer of its own), the forms the compute package built (m2v-compute
/// hwtest-icb-compute-spec.txt): handle_table, a kernel reading an argument buffer's texture and buffer through the
/// handle table (out 43); inherited_root_slots, inherited buffers a pointer chosen at run time reads through the root
/// slot table (11, 22; icb_root_slots= the state's root slots where the facade answers them, else -); binds_over_20, 24
/// inherited buffers (300); residual_off16, a buffer bound at offsets 4 and 20, which the driver binds at the base
/// rounded down to 16 with the remainder in the kernel (101, 102, then 105, 106). Per case icb_<case>_value=,
/// icb_<case>_path=, icb_<case>_ok=; failed_cbs=.
struct IcbComputeFormsTest: HWTest {
    let id = "gpu.gm20b.icb_compute_forms"
    let name = "Indirect command buffers' dispatches: handle tables, root slots, 24 binds and unaligned offsets, on raw"
    let subsystem = Subsystem.metal

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct IcbArgs { texture2d<uint> t; device const uint *b; };
    kernel void icb_handle_table(constant IcbArgs &a [[buffer(0)]], device uint *out [[buffer(1)]]) {
        out[0] = a.t.read(uint2(1, 2)).x + a.b[3];
    }
    kernel void icb_root_slots(device uint *x [[buffer(1)]], device uint *y [[buffer(2)]],
                               device uint *out [[buffer(3)]], uint i [[thread_position_in_grid]]) {
        device uint *p = i ? y : x;
        out[i] = p[i];
    }
    kernel void icb_binds(\((0 ..< 24).map { "device const uint *b\($0) [[buffer(\($0))]]," }.joined(separator: " "))
                          device uint *out [[buffer(24)]]) {
        out[0] = \((0 ..< 24).map { "b\($0)[0]" }.joined(separator: " + "));
    }
    kernel void icb_residual(device const uint *b [[buffer(1)]], device uint *out [[buffer(2)]]) {
        out[0] = b[0];
        out[1] = b[1];
    }
    """

    struct Ran {
        var words: [UInt32] = []
        var path = "-"
    }

    /// One command buffer executing a one-dispatch indirect command buffer of `state` over `threads`; `bind` sets the
    /// encoder's buffers (inherited) or the command's own (`inherit` false)
    static func execute(
        _ gpu: GPU, _ state: MTLComputePipelineState, threads: Int, inherit: Bool,
        bind: (MTLComputeCommandEncoder, MTLIndirectComputeCommand) -> Void
    ) async throws -> Ran {
        let d = MTLIndirectCommandBufferDescriptor()
        d.commandTypes = .concurrentDispatch
        d.inheritBuffers = inherit
        d.inheritPipelineState = false
        d.maxKernelBufferBindCount = inherit ? 0 : 31
        guard let icb = gpu.device.makeIndirectCommandBuffer(descriptor: d, maxCommandCount: 1, options: []) else {
            throw TestFailure("no indirect command buffer")
        }
        let command = icb.indirectComputeCommandAt(0)
        command.setComputePipelineState(state)
        command.concurrentDispatchThreads(
            MTLSize(width: threads, height: 1, depth: 1),
            threadsPerThreadgroup: MTLSize(width: threads, height: 1, depth: 1)
        )
        var ran = Ran()
        ran.path = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            bind(encoder, command)
            encoder.executeCommandsInBuffer(icb, range: 0 ..< 1)
            encoder.endEncoding()
        }.path
        return ran
    }

    static func words(_ buffer: MTLBuffer, _ count: Int) -> [UInt32] {
        Array(UnsafeBufferPointer(start: buffer.contents().bindMemory(to: UInt32.self, capacity: count), count: count))
    }

    static func state(_ gpu: GPU, _ library: MTLLibrary, _ name: String) async throws -> MTLComputePipelineState {
        let d = MTLComputePipelineDescriptor()
        d.computeFunction = library.makeFunction(name: name)
        d.supportIndirectCommandBuffers = true
        return try await gpu.device.makeComputePipelineState(descriptor: d, options: []).0
    }

    static func handleTable(_ gpu: GPU, _ library: MTLLibrary) async throws -> Ran {
        let state = try await state(gpu, library, "icb_handle_table")
        let texture = try gpu.texture(.r32Uint, 4, 4, usage: [.shaderRead])
        try await gpu.upload(
            (0 ..< 16).flatMap { i in withUnsafeBytes(of: UInt32(16 * (i / 4) + i % 4)) { Array($0) } }, to: texture,
            bytesPerRow: 16
        )
        let table = try gpu.buffer([UInt32]([7, 8, 9, 10]))
        guard let function = library.makeFunction(name: "icb_handle_table") else { throw TestFailure("no kernel") }
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let arguments = try gpu.buffer(encoder.encodedLength)
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(texture, index: 0)
        encoder.setBuffer(table, offset: 0, index: 1)
        let out = try gpu.buffer(64)
        var ran = try await execute(gpu, state, threads: 1, inherit: false) { compute, command in
            command.setKernelBuffer(arguments, offset: 0, at: 0)
            command.setKernelBuffer(out, offset: 0, at: 1)
            compute.useResource(texture, usage: .read)
            compute.useResource(table, usage: .read)
            compute.useResource(arguments, usage: .read)
            compute.useResource(out, usage: .write)
        }
        ran.words = words(out, 1)
        return ran
    }

    static func rootSlots(_ gpu: GPU, _ library: MTLLibrary) async throws -> (Ran, String) {
        let state = try await state(gpu, library, "icb_root_slots")
        let x = try gpu.buffer([UInt32]([11, 12])), y = try gpu.buffer([UInt32]([21, 22])), out = try gpu.buffer(64)
        var ran = try await execute(gpu, state, threads: 2, inherit: true) { compute, _ in
            compute.setBuffer(x, offset: 0, index: 1)
            compute.setBuffer(y, offset: 0, index: 2)
            compute.setBuffer(out, offset: 0, index: 3)
        }
        ran.words = words(out, 2)
        let probe = NSSelectorFromString("switchMetalObject")
        let object = (state as? NSObject).flatMap {
            $0.responds(to: probe) ? $0.perform(probe)?.takeUnretainedValue() : nil
        }
        return (ran, ((object as? [String: Any])?["root_slots"] as? NSNumber).map { "\($0)" } ?? "-")
    }

    static func bindsOver20(_ gpu: GPU, _ library: MTLLibrary) async throws -> Ran {
        let state = try await state(gpu, library, "icb_binds")
        let buffers = try (0 ..< 24).map { try gpu.buffer([UInt32(1 + $0)]) }
        let out = try gpu.buffer(64)
        var ran = try await execute(gpu, state, threads: 1, inherit: true) { compute, _ in
            for (index, buffer) in buffers.enumerated() { compute.setBuffer(buffer, offset: 0, index: index) }
            compute.setBuffer(out, offset: 0, index: 24)
        }
        ran.words = words(out, 1)
        return ran
    }

    static func residual(_ gpu: GPU, _ library: MTLLibrary, offset: Int) async throws -> Ran {
        let state = try await state(gpu, library, "icb_residual")
        let words16 = try gpu.buffer((0 ..< 16).map { UInt32(100 + $0) })
        let out = try gpu.buffer(64)
        var ran = try await execute(gpu, state, threads: 1, inherit: true) { compute, _ in
            compute.setBuffer(words16, offset: offset, index: 1)
            compute.setBuffer(out, offset: 0, index: 2)
        }
        ran.words = words(out, 2)
        return ran
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let cases: [(String, [UInt32])] = [
            ("handle_table", [43]), ("inherited_root_slots", [11, 22]), ("binds_over_20", [300]),
            ("residual_off16_4", [101, 102]), ("residual_off16_20", [105, 106]),
        ]
        for (key, want) in cases {
            do {
                let ran: Ran
                switch key {
                case "handle_table": ran = try await Self.handleTable(gpu, library)
                case "inherited_root_slots":
                    let slots: String
                    (ran, slots) = try await Self.rootSlots(gpu, library)
                    metrics.set("icb_root_slots", slots)
                case "binds_over_20": ran = try await Self.bindsOver20(gpu, library)
                default: ran = try await Self.residual(gpu, library, offset: key.hasSuffix("_4") ? 4 : 20)
                }
                let ok = ran.words == want && ran.path == "gm20b"
                metrics.set("icb_\(key)_value", ran.words.map(String.init).joined(separator: "/"))
                metrics.set("icb_\(key)_path", String(ran.path.prefix(60)))
                metrics.set("icb_\(key)_ok", ok)
                checks.append(("\(key): \(want.map(String.init).joined(separator: "/")) on the driver", ok))
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
