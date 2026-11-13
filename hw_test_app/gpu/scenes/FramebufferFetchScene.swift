import Foundation
import HWTestCore
import Metal

struct FramebufferFetchScene: GPUScene {
    let id = "gpu.raw.framebuffer_fetch"
    let name = "Ordered framebuffer fetch, discard, render-target transitions and copy visibility"

    private static let size = 64
    private static let steps = 12
    // the interlock's stress: 64 full-screen triangles in one draw whose odd steps discard the left half, and 64 instances
    private static let stressSteps = 64

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let indices = try gpu.buffer((0..<(Self.stressSteps * 3 + 3)).map(UInt16.init))
        let indirect = try gpu.buffer([UInt32(Self.steps * 3), 1, 0, 0])
        for format in [TexelFormat.rgba8Unorm, .bgra8Unorm, .bgra8Srgb, .rgba16Float] {
            let target = try gpu.texture(format.metal, Self.size, Self.size)
            let copy = try gpu.texture(format.metal, Self.size, Self.size, usage: [.shaderRead])
            let fetch = try gpu.render(vertex: "fetch_vertex", fragment: "fetch_order_fragment", format: format.metal)
            let region = try gpu.render(vertex: "fetch_vertex", fragment: "fetch_region_fragment", format: format.metal)
            let blend = try gpu.render(vertex: "fetch_vertex", fragment: "solid_fragment", format: format.metal) {
                $0.colorAttachments[0].isBlendingEnabled = true
                $0.colorAttachments[0].sourceRGBBlendFactor = .one
                $0.colorAttachments[0].destinationRGBBlendFactor = .one
                $0.colorAttachments[0].sourceAlphaBlendFactor = .zero
                $0.colorAttachments[0].destinationAlphaBlendFactor = .one
            }
            for mode in 0..<7 {
                let steps = mode >= 5 ? Self.stressSteps : Self.steps
                let key = "\(format.rawValue.lowercased())_\(mode)"
                let before = RawFacade.counters(of: gpu.device)
                let timing = try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.125, green: 0.25, blue: 0.5, alpha: 1)
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(mode == 5 ? region : fetch)
                    var discard: UInt32 = mode == 3 ? 1 : 0
                    var rule = SIMD2<UInt32>(1, UInt32(Self.size / 2))
                    if mode == 5 {
                        encoder.setFragmentBytes(&rule, length: 8, index: 0)
                    } else {
                        encoder.setFragmentBytes(&discard, length: 4, index: 0)
                    }
                    if mode == 1 {
                        encoder.drawIndexedPrimitives(
                            type: .triangle, indexCount: Self.steps * 3, indexType: .uint16,
                            indexBuffer: indices, indexBufferOffset: 0)
                    } else if mode == 2 || mode == 6 {
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: steps)
                    } else if mode == 4 {
                        encoder.drawPrimitives(type: .triangle, indirectBuffer: indirect, indirectBufferOffset: 0)
                    } else {
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: steps * 3)
                    }
                    encoder.setRenderPipelineState(blend)
                    var addition = SIMD4<Float>(0.125, 0.25, 0.0625, 0)
                    encoder.setFragmentBytes(&addition, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    encoder.setRenderPipelineState(fetch)
                    encoder.setFragmentBytes(&discard, length: 4, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: steps * 3, vertexCount: 3)
                    encoder.endEncoding()
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(
                        from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                        sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1), to: copy,
                        destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
                    blit.endEncoding()
                }
                let after = RawFacade.counters(of: gpu.device)
                result.metrics.set("path_\(key)", timing.path)
                result.metrics.set("gpu_ms_\(key)", timing.gpuMilliseconds, digits: 3)
                result.metrics.set("cpu_encode_ms_\(key)", timing.encodeMilliseconds, digits: 3)
                result.metrics.set("cpu_commit_ms_\(key)", timing.commitMilliseconds, digits: 3)
                if RawFacade.kind(of: gpu.device) == "raw" {
                    result.expect(timing.path == "raw", "\(key) did not execute entirely raw: \(timing.path)")
                    let draws = (after["interlock_draws"] ?? 0) - (before["interlock_draws"] ?? 0)
                    result.metrics.set("interlock_\(key)", draws)
                    result.expect(
                        draws >= 2, "\(key) has no confirmed native interlock draws; diagnostic counters required")
                    let ilk = (after["fetch_interlocked"] ?? 0) - (before["fetch_interlocked"] ?? 0)
                    result.metrics.set("ilk_\(key)", ilk)
                    result.expect(ilk >= 2, "\(key): the fetch line's ilk counted no interlocked fetching draw")
                }
                let bytes = try await gpu.read(copy, bytesPerPixel: format.bytesPerTexel)
                let expected =
                    mode == 5
                    ? Self.halves(
                        left: Self.expected(format, discardOdd: true, steps: steps),
                        right: Self.expected(format, discardOdd: false, steps: steps))
                    : Patterns.repeated(
                        Self.expected(format, discardOdd: mode == 3, steps: steps), count: Self.size * Self.size)
                result.compare(
                    key, bytes: bytes, expected: expected, format: format, width: Self.size, height: Self.size,
                    tolerance: format == .rgba16Float ? 0.002 : 3.0 / 255)
                let source = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
                result.compare(
                    "copy_\(key)", bytes: bytes, expected: source, format: format, width: Self.size, height: Self.size,
                    tolerance: 0)
            }
        }
        return result
    }

    private static func halves(left: [UInt8], right: [UInt8]) -> [UInt8] {
        (0..<(size * size)).flatMap { $0 % size < size / 2 ? left : right }
    }

    private static func expected(_ format: TexelFormat, discardOdd: Bool, steps: Int) -> [UInt8] {
        func stored(_ values: [Float]) -> [Float] {
            let decoded = format.decode(format.encode(values)[...])
            return decoded.enumerated().map {
                format == .bgra8Srgb && $0.offset < 3 ? TexelFormat.linear($0.element) : $0.element
            }
        }
        func fetch(_ previous: [Float], _ step: Int) -> [Float] {
            let next = [Float((step * 3 + 1) % 7), Float((step * 5 + 2) % 7), Float((step + 3) % 7)]
            return stored((0..<3).map { previous[$0] * 0.5 + next[$0] / 16 } + [1])
        }
        var value = stored([0.125, 0.25, 0.5, 1])
        for step in 0..<steps where !discardOdd || step % 2 == 0 { value = fetch(value, step) }
        value = stored([value[0] + 0.125, value[1] + 0.25, value[2] + 0.0625, 1])
        return format.encode(fetch(value, steps))
    }
}
