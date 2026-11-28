import Foundation
import HWTestCore
import Metal

/// gpu.gm20b.bytes_address's draw (shaders/BytesAddress.metal): a full-target quad whose vertex function compares the
/// addresses of slots 5 and 6 and colours the target with slot 5's colour where they differ, black where they are equal.
/// Two setVertexBytes bindings are two addresses (magenta); one MTLBuffer bound at one offset to both is one (black),
/// which only real addresses in the holes give: a translation that folded the compare would draw magenta there too.
public enum BytesAddress {
    public static let colour = SIMD4<Float>(1, 0, 1, 1), other = SIMD4<Float>(0, 1, 0, 1)

    /// The target's first pixel, RGBA8, and the run's path; `same`: slots 5 and 6 bind one buffer at one offset
    public static func run(_ gpu: GPU, same: Bool = false) async throws -> (pixel: [UInt8], path: String) {
        let pipeline = try gpu.render(
            vertex: "bytes_address_vertex", fragment: "bytes_address_fragment", format: .rgba8Unorm
        )
        let target = try gpu.texture(.rgba8Unorm, 4, 4)
        let shared = try gpu.buffer(64)
        shared.contents().storeBytes(of: colour, as: SIMD4<Float>.self)
        let ran = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            var corners = GPU.fullScreen, left = colour, right = other
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            if same {
                e.setVertexBuffer(shared, offset: 0, index: 5)
                e.setVertexBuffer(shared, offset: 0, index: 6)
            } else {
                e.setVertexBytes(&left, length: 16, index: 5)
                e.setVertexBytes(&right, length: 16, index: 6)
            }
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
        }
        return (Array(try await gpu.read(target, bytesPerPixel: 4).prefix(4)), ran.path)
    }
}
