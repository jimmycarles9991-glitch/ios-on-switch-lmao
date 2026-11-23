import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A layered pass (renderTargetArrayLength 2) of a Depth32Float array beside a Stencil8 array, cleared to 0.25 and 0x33 and
/// stored: both layers of each read back hold the clears, on the driver (a layered pair was pass.ds, m2v's). The driver renders
/// layers on both builds (21H461 answers layered rendering above its A10X: NativeAnswers).
struct SeparateZetaLayeredTest: HWTest {
    let id = "gpu.gm20b.separate_zeta_layered"
    let name = "A layered pass of Depth32Float and Stencil8 arrays clears every layer, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let n = 16
        func array(_ format: MTLPixelFormat) throws -> MTLTexture {
            let d = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: n, height: n, mipmapped: false)
            d.textureType = .type2DArray
            d.arrayLength = 2
            d.storageMode = .private
            d.usage = [.renderTarget]
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                throw TestFailure("no \(format.rawValue) array")
            }
            return t
        }
        let depth = try array(.depth32Float), stencil = try array(.stencil8)
        let depthBytes = try gpu.buffer(2 * n * n * 4), stencilBytes = try gpu.buffer(2 * n * n)
        let timing = try await gpu.run(tolerateError: true) { commands in
            let p = MTLRenderPassDescriptor()
            p.renderTargetArrayLength = 2
            p.renderTargetWidth = n
            p.renderTargetHeight = n
            p.depthAttachment.texture = depth
            p.depthAttachment.loadAction = .clear
            p.depthAttachment.clearDepth = 0.25
            p.depthAttachment.storeAction = .store
            p.stencilAttachment.texture = stencil
            p.stencilAttachment.loadAction = .clear
            p.stencilAttachment.clearStencil = 0x33
            p.stencilAttachment.storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
            let b = commands.makeBlitCommandEncoder()!
            for slice in 0 ..< 2 {
                b.copy(
                    from: depth, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: depthBytes,
                    destinationOffset: slice * n * n * 4, destinationBytesPerRow: n * 4,
                    destinationBytesPerImage: n * n * 4
                )
                b.copy(
                    from: stencil, sourceSlice: slice, sourceLevel: 0, sourceOrigin: MTLOrigin(x: 0, y: 0, z: 0),
                    sourceSize: MTLSize(width: n, height: n, depth: 1), to: stencilBytes,
                    destinationOffset: slice * n * n,
                    destinationBytesPerRow: n, destinationBytesPerImage: n * n
                )
            }
            b.endEncoding()
        }
        let d = depthBytes.contents().bindMemory(to: Float.self, capacity: 2 * n * n)
        let s = stencilBytes.contents().bindMemory(to: UInt8.self, capacity: 2 * n * n)
        let depthWrong = (0 ..< (2 * n * n)).filter { d[$0] != 0.25 }.count
        let stencilWrong = (0 ..< (2 * n * n)).filter { s[$0] != 0x33 }.count
        var metrics = Metrics()
        metrics.set("path", timing.path)
        metrics.set("depth_wrong", depthWrong)
        metrics.set("stencil_wrong", stencilWrong)
        metrics.set("layer1", "\(d[n * n])/\(s[n * n])")
        var failures: [String] = []
        if timing.path != "gm20b" { failures.append("the pass went \(timing.path)") }
        if depthWrong != 0 { failures.append("\(depthWrong) depth texels not 0.25") }
        if stencilWrong != 0 { failures.append("\(stencilWrong) stencil texels not 0x33") }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
