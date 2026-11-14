import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.zcull's arms past a one-sample, one-layer, cleared depth (raw_zcull.h): the near quad over the left half and
/// the far quads over everything again, on a 4x multisampled depth resolved into the target (`msaa4x`), on a two-layer
/// array whose layers each take the quads (`layered`), and on a depth one pass clears and stores and the next pass of
/// the same command buffer loads (`loaded`, the second pass drawing only the far quads). `<arm>_wrong`: channels off
/// the expected colours; `<arm>_passes`: regions the facade programmed for the arm (at least one, two for `loaded`);
/// `<arm>_error`: the arm could not run.
extension ZcullTest {
    struct ArmContext {
        let gpu: GPU
        let target: MTLTexture
        let width: Int, height: Int
        let state: (MTLCompareFunction) -> MTLDepthStencilState
    }

    func arms(_ c: ArmContext, _ metrics: inout Metrics, _ wrong: inout [String], _ paths: inout Set<String>) async {
        for (name, regions, run) in [
            ("msaa4x", 1, msaa4x), ("layered", 1, layered), ("loaded", 2, loaded),
        ] as [(String, Int, (ArmContext) async throws -> (Int, String))] {
            let before = RawFacade.counters(of: c.gpu.device)
            let ran: (Int, String)
            do {
                ran = try await run(c)
            } catch {
                metrics.set("\(name)_error", String(describing: error).prefix(80).description)
                wrong.append("\(name)_error")
                continue
            }
            let (bad, path) = ran
            let after = RawFacade.counters(of: c.gpu.device)
            let passes = (after["zcull_passes"] ?? 0) - (before["zcull_passes"] ?? 0)
            metrics.set("\(name)_wrong", bad)
            metrics.set("\(name)_passes", passes)
            paths.insert(path)
            if bad != 0 { wrong.append(name) }
            if passes < regions { wrong.append("\(name)_passes") }
        }
    }

    /// Channels off the near colour over the left half and the far one over the right, every third texel
    static func misses(_ raster: Raster, _ w: Int, _ h: Int) -> Int {
        var bad = 0
        for y in stride(from: 0, to: h, by: 3) {
            for x in stride(from: 0, to: w, by: 3) {
                let want = x < w / 2 ? red : green
                for ch in 0..<4 where abs(raster[x, y, ch] - want[ch]) > 0.002 { bad += 1 }
            }
        }
        return bad
    }

    static func quads(
        _ e: MTLRenderCommandEncoder, _ c: ArmContext, near: Bool, instances: Int = 1
    ) {
        let (w, h) = (c.width, c.height)
        var draws: [([SIMD4<Float>], SIMD4<Float>, Int)] = []
        if near { draws.append((GPU.pixelQuad(0, 0, w / 2, h, width: w, height: h, z: 0.25), red, 1)) }
        draws.append((GPU.pixelQuad(0, 0, w, h, width: w, height: h, z: 0.75), green, farQuads))
        e.setDepthStencilState(c.state(.less))
        for (corners, colour, count) in draws {
            var corners = corners
            var colours = [SIMD4<Float>](repeating: colour, count: max(instances, 1))
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setVertexBytes(&colours, length: MemoryLayout<SIMD4<Float>>.stride * colours.count, index: 1)
            var single = colour
            e.setFragmentBytes(&single, length: 16, index: 0)
            for _ in 0..<count {
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: instances)
            }
        }
    }

    func msaa4x(_ c: ArmContext) async throws -> (Int, String) {
        let gpu = c.gpu
        let colour = try gpu.texture(
            .bgra8Unorm, c.width, c.height, usage: [.renderTarget], samples: 4, storage: .memoryless)
        let depth = try gpu.texture(
            .depth32Float, c.width, c.height, usage: [.renderTarget], samples: 4, storage: .memoryless)
        let pipeline = try gpu.render(
            fragment: "exact_fragment", format: .bgra8Unorm, samples: 4, depth: .depth32Float)
        let ran = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].resolveTexture = c.target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            Self.quads(e, c, near: true)
            e.endEncoding()
        }
        return (Self.misses(try await gpu.raster(c.target, .bgra8Unorm), c.width, c.height), ran.path)
    }

    func layered(_ c: ArmContext) async throws -> (Int, String) {
        let gpu = c.gpu
        func array(_ format: MTLPixelFormat, _ usage: MTLTextureUsage) throws -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: c.width, height: c.height, mipmapped: false)
            d.textureType = .type2DArray
            d.arrayLength = 2
            d.usage = usage
            d.storageMode = .private
            guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no 2-layer \(format)") }
            return t
        }
        let colour = try array(.bgra8Unorm, [.renderTarget, .shaderRead])
        let depth = try array(.depth32Float, [.renderTarget])
        let pipeline = try gpu.render(
            vertex: "layered_vertex", fragment: "flat_color_fragment", format: .bgra8Unorm, depth: .depth32Float
        ) { $0.inputPrimitiveTopology = .triangle }
        let ran = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.renderTargetArrayLength = 2
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            Self.quads(e, c, near: true, instances: 2)
            e.endEncoding()
        }
        var bad = 0
        for slice in 0..<2 {
            bad += Self.misses(try await gpu.raster(colour, .bgra8Unorm, slice: slice), c.width, c.height)
        }
        return (bad, ran.path)
    }

    func loaded(_ c: ArmContext) async throws -> (Int, String) {
        let gpu = c.gpu
        let depth = try gpu.texture(.depth32Float, c.width, c.height, usage: [.renderTarget])
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let ran = try await gpu.run { commands in
            for first in [true, false] {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = c.target
                pass.colorAttachments[0].loadAction = first ? .clear : .load
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = first ? .clear : .load
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .store
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                if first {
                    let (w, h) = (c.width, c.height)
                    e.setDepthStencilState(c.state(.less))
                    GPU.draw(e, corners: GPU.pixelQuad(0, 0, w / 2, h, width: w, height: h, z: 0.25), color: Self.red)
                } else {
                    Self.quads(e, c, near: false)
                }
                e.endEncoding()
            }
        }
        return (Self.misses(try await gpu.raster(c.target, .bgra8Unorm), c.width, c.height), ran.path)
    }
}
