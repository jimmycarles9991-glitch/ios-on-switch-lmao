import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A Depth16Unorm texture and a Stencil8 texture as one pass's attachments (pass.ds: GM20B has no 16-bit depth zeta with
/// stencil, so the raw path renders a Depth32Float_Stencil8 and converts the depth in and out by draws). The first pass
/// clears depth to 0.25 (Metal's code 0x3fff), writes depth 0.5 and stencil 0x5A over the left half and stores both; the
/// second loads both, draws red at 0.2 where the stencil is 0x5A and stores them again. Every depth code read back after
/// the second pass is the one the first stored (the load and store through the float depth keep each code), the right
/// half 0x3fff, the colour red on the left alone, the stencil 0x5A on the left; both passes on the raw path.
struct SeparateDepth16StencilTest: HWTest {
    let id = "gpu.raw.separate_depth16_stencil"
    let name = "Depth16Unorm and Stencil8 textures as one pass's attachments, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let n = 32
        let colour = try gpu.texture(.rgba8Unorm, n, n)
        let depth = try gpu.texture(.depth16Unorm, n, n, usage: [.renderTarget])
        let stencil = try gpu.texture(.stencil8, n, n, usage: [.renderTarget])
        let state = try gpu.render(
            fragment: "solid_fragment", format: .rgba8Unorm, depth: .depth16Unorm, stencil: .stencil8)
        func ds(
            _ compare: MTLCompareFunction, write: Bool, stencil: MTLCompareFunction, replace: Bool
        ) throws
            -> MTLDepthStencilState
        {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = compare
            d.isDepthWriteEnabled = write
            let face = MTLStencilDescriptor()
            face.stencilCompareFunction = stencil
            face.depthStencilPassOperation = replace ? .replace : .keep
            d.frontFaceStencil = face
            d.backFaceStencil = face
            guard let s = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
            return s
        }
        let first = try ds(.always, write: true, stencil: .always, replace: true)
        let second = try ds(.less, write: false, stencil: .equal, replace: false)
        func quad(_ e: MTLRenderCommandEncoder, x0: Float, x1: Float, z: Float, _ rgba: SIMD4<Float>) {
            var corners: [SIMD4<Float>] = [[x0, -1, z, 1], [x1, -1, z, 1], [x0, 1, z, 1], [x1, 1, z, 1]]
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            var c = rgba
            e.setFragmentBytes(&c, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        }
        func pass(_ load: Bool) -> MTLRenderPassDescriptor {
            let p = MTLRenderPassDescriptor()
            p.colorAttachments[0].texture = colour
            p.colorAttachments[0].loadAction = load ? .load : .clear
            p.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            p.colorAttachments[0].storeAction = .store
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = load ? .load : .clear
            p.depthAttachment.clearDepth = 0.25
            p.depthAttachment.storeAction = .store
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = load ? .load : .clear
            p.stencilAttachment.storeAction = .store
            return p
        }
        let afterFirst = try gpu.buffer(n * n * 2), afterSecond = try gpu.buffer(n * n * 2)
        let stencilBytes = try gpu.buffer(n * n)
        func readDepth(_ commands: MTLCommandBuffer, into out: MTLBuffer) {
            let b = commands.makeBlitCommandEncoder()!
            b.copy(
                from: depth, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                sourceSize: MTLSize(width: n, height: n, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: n * 2, destinationBytesPerImage: n * n * 2)
            b.endEncoding()
        }
        var paths: [String] = []
        paths.append(
            try await gpu.run { commands in
                let e = commands.makeRenderCommandEncoder(descriptor: pass(false))!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(first)
                e.setStencilReferenceValue(0x5A)
                quad(e, x0: -1, x1: 0, z: 0.5, [0, 0, 1, 1])
                e.endEncoding()
                readDepth(commands, into: afterFirst)
            }.path)
        paths.append(
            try await gpu.run { commands in
                let e = commands.makeRenderCommandEncoder(descriptor: pass(true))!
                e.setRenderPipelineState(state)
                e.setDepthStencilState(second)
                e.setStencilReferenceValue(0x5A)
                quad(e, x0: -1, x1: 1, z: 0.2, [1, 0, 0, 1])
                e.endEncoding()
                readDepth(commands, into: afterSecond)
                let b = commands.makeBlitCommandEncoder()!
                b.copy(
                    from: stencil, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencilBytes, destinationOffset: 0,
                    destinationBytesPerRow: n, destinationBytesPerImage: n * n)
                b.endEncoding()
            }.path)
        let pixels = try await gpu.read(colour, bytesPerPixel: 4)
        let d1 = afterFirst.contents().bindMemory(to: UInt16.self, capacity: n * n)
        let d2 = afterSecond.contents().bindMemory(to: UInt16.self, capacity: n * n)
        let s = stencilBytes.contents().bindMemory(to: UInt8.self, capacity: n * n)
        var colourWrong = 0, changed = 0, clearWrong = 0, stencilWrong = 0
        for y in 0..<n {
            for x in 0..<n {
                let i = x + n * y, left = x < n / 2
                let want: [UInt8] = left ? [255, 0, 0, 255] : [0, 0, 0, 255]
                colourWrong += Array(pixels[4 * i..<4 * i + 4]) == want ? 0 : 1
                changed += d1[i] == d2[i] ? 0 : 1
                if !left { clearWrong += d2[i] == 0x3fff ? 0 : 1 }
                stencilWrong += s[i] == (left ? 0x5A : 0) ? 0 : 1
            }
        }
        var metrics = Metrics()
        metrics.set("paths", paths.joined(separator: ","))
        metrics.set("colour_wrong", colourWrong)
        metrics.set("depth_changed", changed)
        metrics.set("clear_wrong", clearWrong)
        metrics.set("stencil_wrong", stencilWrong)
        metrics.set("left_right", String(format: "%04x/%04x->%04x/%04x", d1[2], d1[n - 2], d2[2], d2[n - 2]))
        var failures: [String] = []
        if paths.contains(where: { $0 != "raw" }) { failures.append("a pass went off the raw path") }
        if colourWrong != 0 { failures.append("\(colourWrong) colour pixels wrong") }
        if changed != 0 { failures.append("\(changed) depth codes changed by a load and store") }
        if clearWrong != 0 { failures.append("\(clearWrong) cleared codes not 0x3fff") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) stencil texels wrong") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
