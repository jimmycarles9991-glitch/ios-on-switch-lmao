import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A plain buffer of a struct has an encoder (newArgumentEncoderWithBufferIndex:) whose arguments sit where the
/// function's own program reads the members, never nil: plain_struct_kernel's (shaders/ArgumentShapes.metal) answer
/// what an M4 Max's Metal answers (macOS 27.2: an integer scalar its own size, alignment 1 and no id, so `atomic_uint
/// *` is 4/1; a one-component vector, which Metal reflects with no data type, 8 bytes with an id). A dispatch then
/// reads back what the encoders wrote (k's tail, n[1] and m[2], s's h, v's a) and must give the words the M4 Max
/// gave, on the driver. The case fails on a nil or wrong encoder, a wrong word, a dispatch off the
/// raw path, or any command buffer that failed (failed_cbs=).
struct PlainStructEncoderTest: HWTest {
    let id = "gpu.gm20b.plain_struct_encoder"
    let name = "A plain struct buffer's encoder is where its program reads"
    let subsystem = Subsystem.metal

    /// (buffer index, "length/alignment", "id@offset ...")
    static let measured: [(Int, String, String)] = [
        (0, "32/8", "0@20 1@16 2@8"), (1, "4/1", ""), (2, "56/8", "0@48"), (4, "16/8", "0@4"),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var wrong: [String] = []
        do {
            try await exercise(gpu, &metrics, &wrong)
        } catch {
            metrics.set("dispatch", "threw: \(String(describing: error).prefix(200))")
            wrong.append("threw")
        }
        let failed = FailedCounts.judged(
            before: before, after: GM20BFacade.counters(of: gpu.device), own: gpu.paths, &metrics
        )
        if !failed.1 { wrong.append(failed.0) }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        guard let function = gpu.library.makeFunction(name: "plain_struct_kernel") else {
            throw TestFailure("no function")
        }
        var encoders: [MTLArgumentEncoder] = []
        for (index, sized, places) in Self.measured {
            guard let encoder = FunctionArgumentEncoderTest.made(function.makeArgumentEncoder(bufferIndex: index))
            else {
                wrong.append("\(index)=nil")
                continue
            }
            let (gotSized, gotPlaces) = try FunctionArgumentEncoderTest.answer(
                gpu, encoder, ids: FunctionArgumentEncoderTest.ids(places)
            )
            if gotSized != sized || gotPlaces != places {
                wrong.append("buffer\(index)")
                metrics.set("buffer\(index)", "\(gotSized) \(gotPlaces)")
            }
            encoders.append(encoder)
        }
        guard encoders.count == Self.measured.count else { return }
        let k = try gpu.buffer(64), s = try gpu.buffer(64), v = try gpu.buffer(16)
        let counter = try gpu.buffer(16), out = try gpu.buffer(48)
        encoders[0].setArgumentBuffer(k, offset: 0)
        encoders[0].constantData(at: 0).storeBytes(of: Float(2.5), as: Float.self)
        encoders[0].constantData(at: 1).storeBytes(of: Int32(7), as: Int32.self)
        encoders[0].constantData(at: 2).storeBytes(of: Float(1.25), as: Float.self)
        encoders[2].setArgumentBuffer(s, offset: 0)
        encoders[2].constantData(at: 0).storeBytes(of: SIMD4<Float>(1, 2, 3, 0), as: SIMD4<Float>.self)
        encoders[3].setArgumentBuffer(v, offset: 0)
        encoders[3].constantData(at: 0).storeBytes(of: Int32(9), as: Int32.self)
        let pipeline = try gpu.compute("plain_struct_kernel")
        let timing = try await gpu.run { commands in
            let compute = commands.makeComputeCommandEncoder()!
            compute.setComputePipelineState(pipeline)
            compute.setBuffer(k, offset: 0, index: 0)
            compute.setBuffer(counter, offset: 0, index: 1)
            compute.setBuffer(s, offset: 0, index: 2)
            compute.setBuffer(out, offset: 0, index: 3)
            compute.setBuffer(v, offset: 0, index: 4)
            compute.dispatchThreads(
                MTLSize(width: 1, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1)
            )
            compute.endEncoding()
        }
        let words = (0 ..< 12).map { out.contents().load(fromByteOffset: 4 * $0, as: Float.self) }
        let count = counter.contents().load(as: UInt32.self)
        let exact = words == [2.5, 7, 1.25, 0, 1, 2, 3, 0, 9, 0, 0, 0] && count == 1
        metrics.set("dispatch", "\(timing.path)/\(exact ? "exact" : "wrong")")
        if !exact {
            metrics.set("words", words.map { String($0) }.joined(separator: ",") + " counter \(count)")
            wrong.append("dispatch")
        }
        if timing.path != "gm20b" { wrong.append("dispatch_path") }
    }
}
