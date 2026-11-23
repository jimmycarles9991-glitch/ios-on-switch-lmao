import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Subsampled two-plane YCbCr textures of an odd width or height: Metal makes one used only to be read and none it may
/// write (render target, shader write or Unknown usage; an M4 Max answers nil for 8- and 10-bit, packed, sRGB and code
/// formats). The driver answers nil as Metal does, before anything is made, with the error line `raw ycbcr.odd`; the
/// read-only one is its own and drawn raw.
struct YcbcrOddSizeTest: HWTest {
    let id = "gpu.ycbcr_odd"
    let name = "Odd-size subsampled YCbCr textures: nil where Metal makes none, read-only ones drawn raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let shapes: [(format: UInt, fourcc: OSType, width: Int, height: Int)] = [
        (540, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, 3, 3),
        (500, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, 4, 3),
        (541, kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, 2, 3),
        (543, kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, 3, 4),
    ]
    static let writable: [MTLTextureUsage] = [[.shaderRead, .renderTarget], [], [.shaderRead, .shaderWrite]]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        var made: [String] = [], unread: [String] = []
        var readable: [MTLTexture] = []
        for shape in Self.shapes {
            guard
                let buffer = YcbcrCornerTest.pixelBuffer(16, 8, shape.fourcc),
                let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
                let format = MTLPixelFormat(rawValue: shape.format)
            else { throw TestFailure("no 16x8 surface for \(shape.format)") }
            for usage in Self.writable + [[.shaderRead]] {
                let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                    pixelFormat: format, width: shape.width, height: shape.height, mipmapped: false
                )
                descriptor.usage = usage
                let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0)
                let label = "\(shape.format)_\(shape.width)x\(shape.height)_u\(usage.rawValue)"
                if usage == .shaderRead {
                    if let texture { readable.append(texture) } else { unread.append(label) }
                } else if texture != nil {
                    made.append(label)
                }
            }
        }
        let state = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        for texture in readable {
            let target = try gpu.texture(.rgba32Float, texture.width, texture.height)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(texture, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "gm20b" }
        var metrics = Metrics()
        metrics.set("writable_made", made.isEmpty ? "-" : made.joined(separator: ","))
        metrics.set("read_only_nil", unread.isEmpty ? "-" : unread.joined(separator: ","))
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let checks = [
            ("no writable odd-size texture made, as Metal makes none", made.isEmpty),
            ("every read-only odd-size texture made", unread.isEmpty),
            ("the read-only ones drawn on the driver", raw),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
