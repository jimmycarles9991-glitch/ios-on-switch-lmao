import Foundation
import HWTestCore
import Metal

struct PassFusionScene: GPUScene {
    let id = "gpu.raw.pass_fusion"
    let name = "Adjacent passes preserve encoder defaults, empty clears and framebuffer fetch"

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let side = 64
        for (withDepth, linear) in [(false, false), (true, false), (false, true), (true, true)] {
            let renderWidth = linear ? side : 31
            let renderHeight = linear ? side : 29
            let depthFormat: MTLPixelFormat = withDepth ? .depth32Float : .invalid
            let depth = withDepth ? try gpu.texture(depthFormat, side, side, usage: [.renderTarget]) : nil
            let ordinary = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "solid_fragment", format: .rgba8Unorm, depth: depthFormat)
            let fetch = try gpu.render(
                vertex: "fetch_depth_vertex", fragment: "fetch_add_fragment", format: .rgba8Unorm, depth: depthFormat)
            let rejectDescriptor = MTLDepthStencilDescriptor()
            rejectDescriptor.depthCompareFunction = .never
            rejectDescriptor.isDepthWriteEnabled = true
            let reject = gpu.device.makeDepthStencilState(descriptor: rejectDescriptor)!
            var images: [[UInt8]] = []
            for separate in [false, true] {
                let target: MTLTexture
                if linear {
                    let alignment = max(256, gpu.device.minimumLinearTextureAlignment(for: .rgba8Unorm))
                    let row = (side * 4 + alignment - 1) / alignment * alignment
                    let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                        pixelFormat: .rgba8Unorm, width: side, height: side, mipmapped: false)
                    descriptor.storageMode = .shared
                    descriptor.usage = [.renderTarget, .shaderRead]
                    guard let buffer = gpu.device.makeBuffer(length: row * side, options: .storageModeShared),
                        let image = buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: row)
                    else {
                        throw TestFailure("linear pass-fusion target not made")
                    }
                    target = image
                } else {
                    target = try gpu.texture(.rgba8Unorm, side, side)
                }
                let before = RawFacade.counters(of: gpu.device)
                let timing = try await gpu.run { commands in
                    func pass(clear: Bool) -> MTLRenderPassDescriptor {
                        let pass = MTLRenderPassDescriptor()
                        pass.renderTargetWidth = renderWidth
                        pass.renderTargetHeight = renderHeight
                        pass.colorAttachments[0].texture = target
                        pass.colorAttachments[0].loadAction = clear ? .clear : .load
                        pass.colorAttachments[0].clearColor = MTLClearColor(
                            red: 0.125, green: 0.25, blue: 0.5, alpha: 1)
                        pass.colorAttachments[0].storeAction = .store
                        if let depth {
                            pass.depthAttachment.texture = depth
                            pass.depthAttachment.loadAction = clear ? .clear : .load
                            pass.depthAttachment.storeAction = .store
                            pass.depthAttachment.clearDepth = 1
                        }
                        return pass
                    }
                    func boundary() {
                        if separate { commands.makeBlitCommandEncoder()!.endEncoding() }
                    }
                    func draw(
                        _ encoder: MTLRenderCommandEncoder, _ pipeline: MTLRenderPipelineState, _ color: SIMD4<Float>
                    ) {
                        var z: Float = 0.5
                        var color = color
                        encoder.setRenderPipelineState(pipeline)
                        encoder.setVertexBytes(&z, length: 4, index: 0)
                        encoder.setFragmentBytes(&color, length: 16, index: 0)
                        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    }
                    commands.makeRenderCommandEncoder(descriptor: pass(clear: true))!.endEncoding()
                    boundary()
                    let changed = commands.makeRenderCommandEncoder(descriptor: pass(clear: false))!
                    changed.setViewport(
                        MTLViewport(originX: 1, originY: 2, width: 16, height: 18, znear: 0.2, zfar: 0.8))
                    changed.setScissorRect(MTLScissorRect(x: 2, y: 3, width: 5, height: 7))
                    changed.setCullMode(.front)
                    changed.setTriangleFillMode(.lines)
                    changed.setDepthBias(1, slopeScale: 1, clamp: 1)
                    changed.setBlendColor(red: 1, green: 1, blue: 1, alpha: 1)
                    if withDepth { changed.setDepthStencilState(reject) }
                    draw(changed, ordinary, SIMD4(1, 0, 0, 1))
                    changed.endEncoding()
                    boundary()
                    let defaults = commands.makeRenderCommandEncoder(descriptor: pass(clear: false))!
                    draw(defaults, ordinary, SIMD4(0.25, 0.25, 0.25, 1))
                    defaults.endEncoding()
                    boundary()
                    let fetched = commands.makeRenderCommandEncoder(descriptor: pass(clear: false))!
                    draw(fetched, fetch, SIMD4(0.125, 0.125, 0.125, 1))
                    fetched.endEncoding()
                }
                let key =
                    "\(linear ? "linear" : "private")_\(withDepth ? "depth" : "color")_\(separate ? "separate" : "adjacent")"
                result.metrics.set("path_\(key)", timing.path)
                if RawFacade.kind(of: gpu.device) == "raw" {
                    result.expect(timing.path == "raw", "\(key) did not remain raw: \(timing.path)")
                    let after = RawFacade.counters(of: gpu.device)
                    if let first = before["fused_passes"], let last = after["fused_passes"] {
                        let fused = last - first
                        result.metrics.set("fused_\(key)", fused)
                        result.expect(separate ? fused == 0 : fused >= 3, "\(key) unexpected fusion count \(fused)")
                    } else {
                        result.notes.append("fusion route counters unavailable")
                    }
                }
                let bytes = try await gpu.read(target, bytesPerPixel: 4)
                var expected: [UInt8] = []
                for y in 0..<side {
                    for x in 0..<side {
                        expected += TexelFormat.rgba8Unorm.encode(
                            x < renderWidth && y < renderHeight ? [0.375, 0.375, 0.375, 1] : [0.125, 0.25, 0.5, 1])
                    }
                }
                result.compare(
                    key, bytes: bytes, expected: expected, format: .rgba8Unorm,
                    width: side, height: side, tolerance: 2.0 / 255)
                images.append(bytes)
            }
            result.compare(
                "\(linear)_\(withDepth)_twins", bytes: images[0], expected: images[1], format: .rgba8Unorm,
                width: side, height: side, tolerance: 1.0 / 255)
        }
        return result
    }
}
