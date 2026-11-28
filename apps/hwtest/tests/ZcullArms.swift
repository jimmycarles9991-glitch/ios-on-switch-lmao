import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.gm20b.zcull's arms past a one-sample, one-layer, cleared depth (zcull.h): the near quad over the left half and
/// the far quads over everything again, on a 4x multisampled depth resolved into the target (`msaa4x`), on a two-layer
/// array whose layers each take the quads (`layered`), and on a depth one pass clears and stores and a later pass of
/// the same command buffer loads, the second pass drawing only the far quads: right after it, where the facade fuses the
/// two into one pass that keeps its region, or stores and loads it (`fused`), past a blit between them, where the
/// first stores its region and the second loads it (`loaded`), and past a compute encoder and ten clears, where the
/// commit cuts the range between them and the second pass's job loads the region the first's kept for it (`carried`).
/// `<arm>_wrong`: channels off the expected colours; `<arm>_passes`: regions the facade programmed for the arm (at least
/// one, two for `loaded`, `carried` and an unfused `fused`); `<arm>_fused`: passes it fused; `<arm>_loads`: regions
/// loaded (at least one for `loaded` and `carried`); `<arm>_carried`: of those, regions an earlier job stored (at least
/// one for `carried`); `<arm>_error`: the arm could not run.
extension ZcullTest {
    struct ArmContext {
        let gpu: GPU
        let target: MTLTexture
        let width: Int, height: Int
        let state: (MTLCompareFunction) -> MTLDepthStencilState
    }

    func arms(_ c: ArmContext, _ metrics: inout Metrics, _ wrong: inout [String], _ paths: inout Set<String>) async {
        for (name, regions, fusions, loads, carries, run) in [
            ("msaa4x", 1, 0, 0, 0, msaa4x), ("layered", 1, 0, 0, 0, layered),
            ("fused", 2, 1, 0, 0, { try await self.loaded($0, apart: false) }),
            ("loaded", 2, 0, 1, 0, { try await self.loaded($0, apart: true) }),
            ("carried", 2, 0, 1, 1, carried),
        ] as [(String, Int, Int, Int, Int, (ArmContext) async throws -> (Int, String))] {
            let before = GM20BFacade.counters(of: c.gpu.device)
            let ran: (Int, String)
            do {
                ran = try await run(c)
            } catch {
                metrics.set("\(name)_error", String(describing: error).prefix(80).description)
                wrong.append("\(name)_error")
                continue
            }
            let (bad, path) = ran
            let after = GM20BFacade.counters(of: c.gpu.device)
            let delta = { (key: String) in (after[key] ?? 0) - (before[key] ?? 0) }
            let passes = delta("zcull_passes"), fused = delta("fused_passes"), loaded = delta("zcull_loads")
            let carried = delta("zcull_carried")
            metrics.set("\(name)_wrong", bad)
            metrics.set("\(name)_passes", passes)
            if fusions > 0 || loads > 0 {
                metrics.set("\(name)_fused", fused)
                metrics.set("\(name)_loads", loaded)
            }
            paths.insert(path)
            if bad != 0 { wrong.append(name) }
            // a store and a load the fusion made needless: one pass, its region kept
            let keptInOne = fusions > 0 && fused >= fusions && passes >= 1
            if passes < regions && !keptInOne { wrong.append("\(name)_passes") }
            if loaded < loads { wrong.append("\(name)_loads") }
            if carries > 0 {
                metrics.set("\(name)_carried", carried)
                if carried < carries { wrong.append("\(name)_carried") }
            }
        }
    }

    /// Channels off the near colour over the left half and the far one over the right, every third texel
    static func misses(_ raster: Raster, _ w: Int, _ h: Int) -> Int {
        var bad = 0
        for y in stride(from: 0, to: h, by: 3) {
            for x in stride(from: 0, to: w, by: 3) {
                let want = x < w / 2 ? red : green
                for ch in 0 ..< 4 where abs(raster[x, y, ch] - want[ch]) > 0.002 { bad += 1 }
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
            for _ in 0 ..< count {
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: instances)
            }
        }
    }

    func msaa4x(_ c: ArmContext) async throws -> (Int, String) {
        let gpu = c.gpu
        let colour = try gpu.texture(
            .bgra8Unorm, c.width, c.height, usage: [.renderTarget], samples: 4, storage: .memoryless
        )
        let depth = try gpu.texture(
            .depth32Float, c.width, c.height, usage: [.renderTarget], samples: 4, storage: .memoryless
        )
        let pipeline = try gpu.render(
            fragment: "exact_fragment", format: .bgra8Unorm, samples: 4, depth: .depth32Float
        )
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
                pixelFormat: format, width: c.width, height: c.height, mipmapped: false
            )
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
        for slice in 0 ..< 2 {
            bad += Self.misses(try await gpu.raster(colour, .bgra8Unorm, slice: slice), c.width, c.height)
        }
        return (bad, ran.path)
    }

    func loaded(_ c: ArmContext, apart: Bool) async throws -> (Int, String) {
        let gpu = c.gpu
        let depth = try gpu.texture(.depth32Float, c.width, c.height, usage: [.renderTarget])
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let between = try gpu.buffer(256)
        let ran = try await gpu.run { commands in
            for first in [true, false] {
                if !first && apart {
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.fill(buffer: between, range: 0 ..< 256, value: 0)
                    blit.endEncoding()
                }
                Self.depthPass(commands, c, depth, pipeline, first: first)
            }
        }
        return (Self.misses(try await gpu.raster(c.target, .bgra8Unorm), c.width, c.height), ran.path)
    }

    func carried(_ c: ArmContext) async throws -> (Int, String) {
        let gpu = c.gpu
        let depth = try gpu.texture(.depth32Float, c.width, c.height, usage: [.renderTarget])
        let filler = try gpu.texture(.bgra8Unorm, 8, 8, usage: [.renderTarget])
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let ran = try await gpu.run { commands in
            // first, so the command buffer is encoded at its commit, where its range is cut into chunks
            commands.makeComputeCommandEncoder()!.endEncoding()
            Self.depthPass(commands, c, depth, pipeline, first: true)
            for _ in 0 ..< 10 {
                let clear = MTLRenderPassDescriptor()
                clear.colorAttachments[0].texture = filler
                clear.colorAttachments[0].loadAction = .clear
                clear.colorAttachments[0].storeAction = .store
                commands.makeRenderCommandEncoder(descriptor: clear)!.endEncoding()
            }
            Self.depthPass(commands, c, depth, pipeline, first: false)
        }
        return (Self.misses(try await gpu.raster(c.target, .bgra8Unorm), c.width, c.height), ran.path)
    }

    /// The first pass clears the target and the depth and draws the near quad over the left half; the second loads both
    /// and draws the far quads over everything
    static func depthPass(
        _ commands: MTLCommandBuffer, _ c: ArmContext, _ depth: MTLTexture, _ pipeline: MTLRenderPipelineState,
        first: Bool
    ) {
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
            GPU.draw(e, corners: GPU.pixelQuad(0, 0, w / 2, h, width: w, height: h, z: 0.25), color: red)
        } else {
            quads(e, c, near: false)
        }
        e.endEncoding()
    }
}
