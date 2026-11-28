import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A one-sample RGBA8Unorm attachment drawn a gradient of exact codes (texel_values, each channel c / 255; red + blue
/// 255 everywhere, so red never equals blue) and resolved by MultisampleResolve into a BGRA8Unorm and into an
/// RGBA8Unorm_sRGB texture: the resolve is a copy of the codes, red and blue swapped into BGRA's byte order and no sRGB
/// step into the sRGB twin, every texel, tolerance 0 (an M4 Max: passoracle.mm's one-sample resolve, and this scene: 0
/// texels off each). Per destination <dst>_off texels off, <dst>_texel the bytes of texel (5, 3) as stored (an M4 Max:
/// bgra8 232/13/23/134, rgba8_srgb 23/13/232/134); every command buffer raw, failed_cbs=0.
struct ResolveOneSampleSwapTest: HWTest {
    let id = "gpu.gm20b.resolve_one_sample_swap"
    let name = "A one-sample RGBA8 resolve copies its codes into BGRA8 and RGBA8 sRGB textures, on raw"
    let subsystem = Subsystem.metal

    static let side = 64
    static let destinations: [(key: String, format: MTLPixelFormat, order: [Int])] = [
        ("bgra8", .bgra8Unorm, [2, 1, 0, 3]), ("rgba8_srgb", .rgba8Unorm_srgb, [0, 1, 2, 3]),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for d in Self.destinations {
            do {
                let bytes = try await Self.resolved(gpu, into: d.format)
                let off = Self.texelsOff(bytes, order: d.order)
                metrics.set(d.key + "_off", off)
                metrics.set(
                    d.key + "_texel", bytes.pixel(5, 3, width: Self.side).map(String.init).joined(separator: "/")
                )
                checks.append(("\(d.key): every texel the source's codes in its byte order", off == 0))
            } catch {
                checks.append(("\(d.key): \(error)", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func codes(_ x: Int, _ y: Int) -> [Int] {
        [4 * x + y % 4, 4 * y + x % 4, 255 - 4 * x - y % 4, 128 + x + y / 2]
    }

    static func resolved(_ gpu: GPU, into format: MTLPixelFormat) async throws -> [UInt8] {
        let n = side
        let values = try gpu.buffer(
            (0 ..< n * n).map { SIMD4<Float>(codes($0 % n, $0 / n).map { Float($0) / 255 }) }
        )
        let state = try gpu.render(fragment: "texel_values", format: .rgba8Unorm)
        let source = try gpu.texture(.rgba8Unorm, n, n, usage: [.renderTarget])
        let destination = try gpu.texture(format, n, n)
        var width = UInt32(n)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = source
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = destination
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(values, offset: 0, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return try await gpu.read(destination, bytesPerPixel: 4)
    }

    /// Texels whose bytes are not the source's codes in `order` (the source channel each byte holds)
    static func texelsOff(_ bytes: [UInt8], order: [Int]) -> Int {
        (0 ..< side * side).filter { at in
            let want = codes(at % side, at / side)
            return order.enumerated().contains { Int(bytes[4 * at + $0.offset]) != want[$0.element] }
        }.count
    }
}
