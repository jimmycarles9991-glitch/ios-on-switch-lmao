import Foundation
import HWTestCore
import Metal

/// What one small command buffer costs on the host clock: encoding it on the CPU, and from just before its commit to
/// its completed handler. A clear pass and a buffer fill, 120 of each, both on raw's path; their GPU work is next to
/// nothing, so submit to complete is the driver's and the kernel's own latency.
struct LatencyScene: GPUScene {
    let id = "gpu.latency"
    let name = "Command buffer latency: CPU encode and submit to complete"

    static let rounds = 120
    static let frameMicroseconds = 16_667.0

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let target = try gpu.texture(.bgra8Unorm, 256, 256)
        let buffer = try gpu.buffer(1 << 16)
        try await measure("clear", gpu, &result) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        try await measure("fill", gpu, &result) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.fill(buffer: buffer, range: 0..<buffer.length, value: 0x5A)
            blit.endEncoding()
        }
        result.expect(buffer.contents().load(as: UInt8.self) == 0x5A, "the fills left the buffer unfilled")
        return result
    }

    private func measure(
        _ label: String, _ gpu: GPU, _ result: inout SceneResult, _ encode: (MTLCommandBuffer) -> Void
    ) async throws {
        var encoding: [Double] = []
        var submitted: [Double] = []
        for _ in 0..<Self.rounds {
            var started: UInt64 = 0
            let timing = try await gpu.run { commands in
                started = DispatchTime.now().uptimeNanoseconds
                encode(commands)
                encoding.append(Double(DispatchTime.now().uptimeNanoseconds - started) / 1000)
            }
            submitted.append(timing.wallMilliseconds * 1000)
        }
        for (name, values) in [("encode_us", encoding), ("submit_us", submitted)] {
            for (quantile, q) in [("p50", 0.5), ("p95", 0.95), ("p99", 0.99)] {
                result.metrics.set("\(label)_\(name)_\(quantile)", Stats.percentile(values, q), digits: 1)
            }
        }
        let p99 = Stats.percentile(submitted, 0.99)
        result.expect(p99 < Self.frameMicroseconds, "\(label): p99 submit to complete \(Int(p99)) us, over a frame")
    }
}
