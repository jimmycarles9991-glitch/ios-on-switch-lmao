import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Each mode's fetching draws twice: inside the pixel interlock (fetch rung 4 forced; <key>), and on the route the
/// driver picks by default (default_<key>), which rule fetch.twin (twin_rule.h) names. Over this 64x64 target the
/// rule takes the barrier twin for a list of up to 32 triangles that is neither instanced nor indirect (modes 0, 1
/// and 3) and for each mode's last one-triangle draw; the instanced (2, 6), indirect (4) and 64-triangle (5) draws
/// stay interlocked.
struct FramebufferFetchScene: GPUScene {
    let id = "gpu.gm20b.framebuffer_fetch"
    let name = "Ordered framebuffer fetch, discard, render-target transitions and copy visibility"

    private static let size = 64
    private static let steps = 12
    // the interlock's stress: 64 full-screen triangles in one draw whose odd steps discard the left half, and 64 instances
    private static let stressSteps = 64
    private static let twinModes: Set<Int> = [0, 1, 3]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let indices = try gpu.buffer((0 ..< (Self.stressSteps * 3 + 3)).map(UInt16.init))
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
            for mode in 0 ..< 7 {
                let steps = mode >= 5 ? Self.stressSteps : Self.steps
                let key = "\(format.rawValue.lowercased())_\(mode)"
                let encode: (MTLCommandBuffer) -> Void = { commands in
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
                            indexBuffer: indices, indexBufferOffset: 0
                        )
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
                        destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
                    )
                    blit.endEncoding()
                }
                let expected =
                    mode == 5
                        ? Self.halves(
                            left: Self.expected(format, discardOdd: true, steps: steps),
                            right: Self.expected(format, discardOdd: false, steps: steps)
                        )
                        : Patterns.repeated(
                            Self.expected(format, discardOdd: mode == 3, steps: steps), count: Self.size * Self.size
                        )
                for interlocked in [true, false] {
                    let label = interlocked ? key : "default_\(key)"
                    let before = GM20BFacade.counters(of: gpu.device)
                    let timing = try await Self.timed(gpu, interlocked: interlocked, encode)
                    let moved = GM20BFacade.counters(of: gpu.device).merging(before) { $0 - $1 }
                    let route: Route = interlocked ? .interlock : Self.twinModes.contains(mode) ? .twin : .lastTwin
                    let facade = GM20BFacade.kind(of: gpu.device) == "gm20b"
                    Self.judged(label, timing, facade ? moved : nil, route, &result)
                    let bytes = try await gpu.read(copy, bytesPerPixel: format.bytesPerTexel)
                    result.compare(
                        label, bytes: bytes, expected: expected, format: format, width: Self.size, height: Self.size,
                        tolerance: format == .rgba16Float ? 0.002 : 3.0 / 255
                    )
                    let source = try await gpu.read(target, bytesPerPixel: format.bytesPerTexel)
                    result.compare(
                        "copy_\(label)", bytes: bytes, expected: source, format: format, width: Self.size,
                        height: Self.size, tolerance: 0
                    )
                }
            }
        }
        if GM20BFacade.kind(of: gpu.device) == "gm20b" {
            let object = gpu.device as AnyObject as! NSObject
            for (key, selector) in [
                ("read_answer", "isFramebufferReadSupported"),
                ("blending_answer", "supportsProgrammableBlending"),
            ] {
                let answer = (object.value(forKey: selector) as? NSNumber)?.intValue ?? -1
                result.metrics.set(key, answer)
                result.expect(answer == 1, "\(selector) answers \(answer)")
            }
        }
        return result
    }

    /// The route a run's fetching draws must take: all inside the interlock (fetch rung 4 forced), all through the twin
    /// rule, or only the last one-triangle draw through it
    private enum Route {
        case interlock, twin, lastTwin
    }

    private static func timed(
        _ gpu: GPU, interlocked: Bool, _ encode: (MTLCommandBuffer) -> Void
    ) async throws -> GPU.Timing {
        if interlocked, let forced = try await GM20BFacade.forcingFetchRung(4, on: gpu.device, {
            try await gpu.run(encode)
        }) {
            return forced
        }
        return try await gpu.run(encode)
    }

    /// A run's times, and on the facade (`moved`: its counters' moves) its path and route
    private static func judged(
        _ label: String, _ timing: GPU.Timing, _ moved: [String: Int]?, _ route: Route, _ result: inout SceneResult
    ) {
        result.metrics.set("path_\(label)", timing.path)
        result.metrics.set("gpu_ms_\(label)", timing.gpuMilliseconds, digits: 3)
        result.metrics.set("cpu_encode_ms_\(label)", timing.encodeMilliseconds, digits: 3)
        result.metrics.set("cpu_commit_ms_\(label)", timing.commitMilliseconds, digits: 3)
        guard let moved else { return }
        result.expect(timing.path == "gm20b", "\(label) did not execute entirely raw: \(timing.path)")
        let draws = moved["interlock_draws"] ?? 0
        result.metrics.set("interlock_\(label)", draws)
        guard route != .interlock else {
            result.expect(draws >= 2, "\(label) has no confirmed native interlock draws; diagnostic counters required")
            let ilk = moved["fetch_interlocked"] ?? 0
            result.metrics.set("ilk_\(label)", ilk)
            result.expect(ilk >= 2, "\(label): the fetch line's ilk counted no interlocked fetching draw")
            return
        }
        result.metrics.set("twin_\(label)", moved["fetch_twin_rule"] ?? 0)
        let (twins, interlocked) = route == .twin ? (2, 0) : (1, 1)
        for (why, held) in FetchRoute.ruleChecks(label, twins: twins, interlocked: interlocked, moved: moved) {
            result.expect(held, why)
        }
    }

    private static func halves(left: [UInt8], right: [UInt8]) -> [UInt8] {
        (0 ..< (size * size)).flatMap { $0 % size < size / 2 ? left : right }
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
            return stored((0 ..< 3).map { previous[$0] * 0.5 + next[$0] / 16 } + [1])
        }
        var value = stored([0.125, 0.25, 0.5, 1])
        for step in 0 ..< steps where !discardOdd || step % 2 == 0 { value = fetch(value, step) }
        value = stored([value[0] + 0.125, value[1] + 0.25, value[2] + 0.0625, 1])
        return format.encode(fetch(value, steps))
    }
}
