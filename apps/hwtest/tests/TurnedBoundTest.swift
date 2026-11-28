import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// A texture of the rotation SPI bound as a texture argument. Such a texture is framebuffer-only (MTLTexture.h: no
/// texture argument), and an M4 Max reads one bound anyway as its memory lies, unturned. The driver draws it from the
/// plane as it lies and logs `raw tex.turned_bound` once; before, the binding sent the command buffer to m2v, which has
/// no texture of a turned plane. Each rotation's draw must complete on the driver within the timeout.
struct TurnedBoundTest: HWTest {
    let id = "gpu.gm20b.turned_bound"
    let name = "A rotated plane bound as a texture argument draws on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let state = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for turn in 1 ... 4 {
            let quarter = turn != 2
            let (w, h) = (24, 16)
            let (pw, ph) = quarter ? (h, w) : (w, h)
            let properties: [IOSurfacePropertyKey: Any] = [
                .width: pw, .height: ph, .bytesPerElement: 4, .bytesPerRow: pw * 4, .allocSize: pw * 4 * ph,
                .pixelFormat: 0x4247_5241,
            ]
            guard let surface = IOSurface(properties: properties) else { throw TestFailure("no surface") }
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: w, height: h, mipmapped: false
            )
            descriptor.usage = [.renderTarget, .shaderRead]
            descriptor.storageMode = .shared
            guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
                return .skip("no rotation SPI on this descriptor")
            }
            descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
            guard let turned = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
                checks.append(("rotation \(turn): made", false))
                continue
            }
            let target = try gpu.texture(.rgba32Float, 4, 4)
            let timing = try await gpu.run(tolerateError: true) { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(turned, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            metrics.set("path_r\(turn)", timing.path)
            checks.append(("rotation \(turn): the draw that reads it completes on the driver", timing.path == "gm20b"))
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
