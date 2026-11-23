import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Blit copies between Depth32Float_Stencil8's X32_Stencil8 view and an R8Uint texture, a byte a texel each way: the
/// stencil written through a buffer copy reads back in R8Uint, and R8Uint's bytes copied in read back as the stencil,
/// the depth beside them as it was.
enum StencilByteCopies {
    static let side = 16

    struct Result {
        var paths: [String] = []
        var outWrong = 0
        var inWrong = 0
        var depthChanged = 0
    }

    static func run(_ gpu: GPU) async throws -> Result {
        let n = side
        let both = try gpu.texture(.depth32Float_stencil8, n, n, usage: [.renderTarget, .shaderRead])
        guard let view = both.makeTextureView(pixelFormat: .x32_stencil8) else {
            throw TestFailure("no X32_Stencil8 view")
        }
        let bytes = try gpu.texture(.r8Uint, n, n, usage: [.shaderRead])
        let stencil = (0 ..< n * n).map { UInt8(truncatingIfNeeded: $0 &* 29 &+ 7) }
        let back = (0 ..< n * n).map { UInt8(truncatingIfNeeded: $0 &* 53 &+ 101) }
        let staging = try gpu.buffer(stencil)
        let size = MTLSize(width: n, height: n, depth: 1)
        var result = Result()
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = both
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.5
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = both
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        result.paths.append(
            try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: staging, sourceOffset: 0, sourceBytesPerRow: n, sourceBytesPerImage: n * n, sourceSize: size,
                    to: both, destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin(),
                    options: .stencilFromDepthStencil
                )
                blit.copy(
                    from: view, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: size, to: bytes,
                    destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
                )
                blit.endEncoding()
            }.path
        )
        result.outWrong = zip(try await gpu.read(bytes, bytesPerPixel: 1), stencil).filter { $0 != $1 }.count
        try await gpu.upload(back, to: bytes, bytesPerRow: n)
        result.paths.append(
            try await gpu.run(tolerateError: true) { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: bytes, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(), sourceSize: size, to: view,
                    destinationSlice: 0, destinationLevel: 0, destinationOrigin: MTLOrigin()
                )
                blit.endEncoding()
            }.path
        )
        let read = try await gpu.read(both, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        result.inWrong = zip(read, back).filter { $0 != $1 }.count
        let depth = try await gpu.read(both, bytesPerPixel: 4, options: .depthFromDepthStencil)
        result.depthChanged = (0 ..< n * n).filter { depth.loaded(as: Float.self, at: $0) != 0.5 }.count
        return result
    }
}
