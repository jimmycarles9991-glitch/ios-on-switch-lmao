import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A multisampled Depth32Float texture and Stencil8 texture as one pass's attachments, resolved (pass.ds took a pair with a
/// resolve to m2v): cleared to 0.25 and 0x33, a quad at depth 0.5 replacing stencil with 0x5A over the left half, then the
/// depth resolved (sample 0) into a one-sample Depth32Float and, where the device resolves stencil (Apple5), the stencil into a
/// one-sample Stencil8. Read back: 0.5 and 0x5A on the left, 0.25 and 0x33 on the right; the pass on the driver.
struct SeparateZetaResolveTest: HWTest {
    let id = "gpu.gm20b.separate_zeta_resolve"
    let name = "A multisampled Depth32Float and Stencil8 pair resolved, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = 16, samples = 4
        let stencilResolves = gpu.device.supportsFamily(.apple5)
        let colour = try gpu.texture(.rgba8Unorm, n, n, samples: samples)
        let depth = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget], samples: samples)
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget], samples: samples)
        let depthOut = try gpu.texture(.depth32Float, n, n, usage: [.renderTarget])
        let stencilOut = try gpu.texture(.stencil8, n, n, usage: [.renderTarget])
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, samples: samples, depth: .depth32Float, stencil: .stencil8
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.stencilCompareFunction = .always
        face.depthStencilPassOperation = .replace
        d.frontFaceStencil = face
        d.backFaceStencil = face
        guard let write = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        let depths = try gpu.buffer(n * n * 4), stencils = try gpu.buffer(n * n)
        let path = try await gpu.run { commands in
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = .clear
            p.colorAttachments[0].storeAction = .dontCare
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = .clear
            p.depthAttachment.clearDepth = 0.25
            p.depthAttachment.storeAction = .multisampleResolve
            p.depthAttachment.resolveTexture = depthOut
            p.depthAttachment.depthResolveFilter = .sample0
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.clearStencil = 0x33
            if stencilResolves {
                p.stencilAttachment.storeAction = .multisampleResolve
                p.stencilAttachment.resolveTexture = stencilOut
            } else {
                p.stencilAttachment.storeAction = .dontCare
            }
            let e = commands.makeRenderCommandEncoder(descriptor: p)!
            e.setRenderPipelineState(state)
            e.setDepthStencilState(write)
            e.setStencilReferenceValue(0x5A)
            var corners: [SIMD4<Float>] = [[-1, -1, 0.5, 1], [0, -1, 0.5, 1], [-1, 1, 0.5, 1], [0, 1, 0.5, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var red = SIMD4<Float>(1, 0, 0, 1)
            e.setFragmentBytes(&red, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
            let b = commands.makeBlitCommandEncoder()!
            b.copy(
                from: depthOut, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: n, height: n, depth: 1), to: depths, destinationOffset: 0,
                destinationBytesPerRow: n * 4, destinationBytesPerImage: n * n * 4
            )
            if stencilResolves {
                b.copy(
                    from: stencilOut, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencils, destinationOffset: 0,
                    destinationBytesPerRow: n, destinationBytesPerImage: n * n
                )
            }
            b.endEncoding()
        }.path
        let z = depths.contents().bindMemory(to: Float.self, capacity: n * n)
        let s = stencils.contents().bindMemory(to: UInt8.self, capacity: n * n)
        var depthWrong = 0, stencilWrong = 0
        for y in 0 ..< n {
            for x in 0 ..< n {
                let left = x < n / 2
                depthWrong += z[y * n + x] == (left ? 0.5 : 0.25) ? 0 : 1
                if stencilResolves { stencilWrong += s[y * n + x] == (left ? 0x5A : 0x33) ? 0 : 1 }
            }
        }
        var metrics = Metrics()
        metrics.set("path", path)
        metrics.set("depth_wrong", depthWrong)
        metrics.set("stencil_wrong", stencilResolves ? "\(stencilWrong)" : "no stencil resolve (Apple5)")
        metrics.set("left_right", String(format: "%.3f/%.3f %02x/%02x", z[2], z[n - 2], s[2], s[n - 2]))
        var failures: [String] = []
        if path != "gm20b" { failures.append("the pass went off the driver (\(path))") }
        if depthWrong != 0 { failures.append("\(depthWrong) resolved depths wrong") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) resolved stencils wrong") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
