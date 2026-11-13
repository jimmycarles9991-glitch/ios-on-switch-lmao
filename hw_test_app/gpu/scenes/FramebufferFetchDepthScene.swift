import Foundation
import HWTestCore
import Metal

struct FramebufferFetchDepthScene: GPUScene {
    let id = "gpu.raw.framebuffer_fetch_depth"
    let name = "Framebuffer fetch depth/stencil writes, multisampling and fixed-function transitions"

    private static let size = 32

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        var packed: [Bool] = []
        for samples in [1, 2, 4, 8] where gpu.device.supportsTextureSampleCount(samples) {
            for stencil in [false, true] {
                do {
                    try await draw(gpu, samples: samples, stencil: stencil, &packed, &result)
                } catch {
                    result.expect(false, "\(Self.key(samples, stencil)): \(GPU.describe(error))")
                }
            }
        }
        if !packed.isEmpty {
            result.metrics.set("packed", "\(packed.filter { $0 }.count)/\(packed.count)")
        }
        return result
    }

    private static func key(_ samples: Int, _ stencil: Bool) -> String {
        "\(stencil ? "depth_stencil" : "depth")_\(samples)x"
    }

    /// One case in a command buffer of its own: a case that throws leaves the other cases' compares
    private func draw(
        _ gpu: GPU, samples: Int, stencil: Bool, _ packed: inout [Bool], _ result: inout SceneResult
    ) async throws {
        let key = Self.key(samples, stencil)
        let format: MTLPixelFormat = stencil ? .depth32Float_stencil8 : .depth32Float
        let depth = try gpu.texture(format, Self.size, Self.size, usage: [.renderTarget], samples: samples)
        let target = try gpu.texture(.rgba8Unorm, Self.size, Self.size, samples: samples)
        let resolved = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let snapshot = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let fetch = try gpu.render(
            vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: .rgba8Unorm,
            samples: samples, depth: format, stencil: stencil ? format : .invalid)
        let ordinary = try gpu.render(
            vertex: "fetch_depth_vertex", fragment: "solid_fragment", format: .rgba8Unorm,
            samples: samples, depth: format, stencil: stencil ? format : .invalid)
        let less = try Self.state(gpu, stencil: stencil, depth: .less, stencilCompare: .always)
        let equalStencil = try Self.state(gpu, stencil: stencil, depth: .less, stencilCompare: .equal)
        let final = try Self.state(gpu, stencil: stencil, depth: .equal, stencilCompare: .equal)
        let before = RawFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            func pass(clear: Bool) -> MTLRenderPassDescriptor {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = clear ? .clear : .load
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = samples == 1 ? .store : .storeAndMultisampleResolve
                if samples > 1 { pass.colorAttachments[0].resolveTexture = resolved }
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = clear ? .clear : .load
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .store
                if stencil {
                    pass.stencilAttachment.texture = depth
                    pass.stencilAttachment.loadAction = clear ? .clear : .load
                    pass.stencilAttachment.clearStencil = 0
                    pass.stencilAttachment.storeAction = .store
                }
                return pass
            }
            func draw(
                _ encoder: MTLRenderCommandEncoder, _ pipeline: MTLRenderPipelineState,
                _ state: MTLDepthStencilState, _ z: Float, _ color: SIMD4<Float>, _ reference: UInt32 = 0
            ) {
                encoder.setRenderPipelineState(pipeline)
                encoder.setDepthStencilState(state)
                encoder.setStencilReferenceValue(reference)
                var depth = z
                var color = color
                encoder.setVertexBytes(&depth, length: 4, index: 0)
                encoder.setFragmentBytes(&color, length: 16, index: 0)
                encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass(clear: true))!
            draw(encoder, fetch, less, 0.75, SIMD4(0.125, 0, 0, 1))
            draw(encoder, fetch, less, 0.625, SIMD4(0, 0.5, 0, -1))
            draw(encoder, fetch, less, 0.875, SIMD4(0, 0.5, 0, 1))
            draw(encoder, ordinary, equalStencil, 0.5, SIMD4(0.25, 0.25, 0.25, 1), 2)
            draw(encoder, fetch, equalStencil, 0.625, SIMD4(0, 0.5, 0, 1), 3)
            draw(encoder, fetch, equalStencil, 0.25, SIMD4(0.125, 0, 0, 1), 4)
            if stencil {
                draw(encoder, fetch, equalStencil, 0.125, SIMD4(0, 0.5, 0, 1), 7)
                draw(encoder, fetch, equalStencil, 0.375, SIMD4(0, 0.5, 0, 1), 6)
            }
            draw(encoder, fetch, equalStencil, 0.1875, SIMD4(0, 0, 0.125, 1), 7)
            encoder.endEncoding()
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: samples == 1 ? target : resolved, sourceSlice: 0, sourceLevel: 0,
                sourceOrigin: MTLOrigin(), sourceSize: MTLSize(width: Self.size, height: Self.size, depth: 1),
                to: snapshot, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin())
            blit.endEncoding()
            let verify = commands.makeRenderCommandEncoder(descriptor: pass(clear: false))!
            draw(verify, ordinary, final, 0.1875, SIMD4(0.625, 0.5, 0.375, 1), 8)
            verify.endEncoding()
        }
        result.metrics.set("path_\(key)", timing.path)
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(timing.path == "raw", "\(key) did not execute entirely raw: \(timing.path)")
            if let fetchPacked = RawFacade.packed(fetch), let ordinaryPacked = RawFacade.packed(ordinary) {
                packed.append(fetchPacked && ordinaryPacked)
                result.expect(packed.last!, "\(key): a state made at its creation, not the pack's")
            }
            let after = RawFacade.counters(of: gpu.device)
            let draws = (after["interlock_draws"] ?? 0) - (before["interlock_draws"] ?? 0)
            result.metrics.set("interlock_\(key)", draws)
            result.expect(draws >= 5, "\(key) has no confirmed native interlock draw coverage")
        }
        for (label, texture, expected) in [
            ("fetch", snapshot, [Float(0.375), 0.25, 0.375, 1]),
            ("stored_depth_stencil", samples == 1 ? target : resolved, [Float(0.625), 0.5, 0.375, 1]),
        ] {
            let bytes = try await gpu.read(texture, bytesPerPixel: 4)
            result.compare(
                "\(key)_\(label)", bytes: bytes,
                expected: Patterns.repeated(TexelFormat.rgba8Unorm.encode(expected), count: Self.size * Self.size),
                format: .rgba8Unorm, width: Self.size, height: Self.size, tolerance: 2.0 / 255)
        }
    }

    private static func state(
        _ gpu: GPU, stencil: Bool, depth: MTLCompareFunction, stencilCompare: MTLCompareFunction
    ) throws -> MTLDepthStencilState {
        let descriptor = MTLDepthStencilDescriptor()
        descriptor.depthCompareFunction = depth
        descriptor.isDepthWriteEnabled = true
        if stencil {
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = stencilCompare
            face.stencilFailureOperation = .incrementClamp
            face.depthFailureOperation = .incrementClamp
            face.depthStencilPassOperation = .incrementClamp
            descriptor.frontFaceStencil = face
            descriptor.backFaceStencil = face
        }
        guard let state = gpu.device.makeDepthStencilState(descriptor: descriptor) else {
            throw TestFailure("makeDepthStencilState returned nil")
        }
        return state
    }
}
