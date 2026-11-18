import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A rotation asked of a texture over no surface plane, private or over a buffer. Metal makes none (an M4 Max: its
/// descriptor validation wants framebufferOnly, then its driver asserts "Buffer texture has invalid rotation"); the raw
/// path answers nil with the error line `raw tex.turned` and asks m2v for no twin (before: tex.shape and a twin asked).
struct TurnedOffSurfaceTest: HWTest {
    let id = "gpu.raw.turned_nil"
    let name = "A rotation over no surface plane: nil, as Metal makes none, and no m2v twin"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 15 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        let needs = {
            ((RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any]) ?? [:]).values.reduce(0) {
                $0 + (($1 as? NSNumber)?.intValue ?? 0)
            }
        }
        let before = needs()
        var made: [String] = []
        let buffer = try gpu.buffer(64 * 32 * 4)
        for turn in 1...4 {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: 64, height: 32, mipmapped: false)
            descriptor.usage = [.renderTarget, .shaderRead]
            guard descriptor.responds(to: NSSelectorFromString("setRotation:")) else {
                return .skip("no rotation SPI on this descriptor")
            }
            descriptor.setValue(NSNumber(value: true), forKey: "framebufferOnly")
            descriptor.setValue(NSNumber(value: turn), forKey: "rotation")
            descriptor.storageMode = .private
            if gpu.device.makeTexture(descriptor: descriptor) != nil { made.append("private_r\(turn)") }
            descriptor.storageMode = .shared
            if buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: 256) != nil {
                made.append("buffer_r\(turn)")
            }
        }
        let asked = needs() - before
        var metrics = Metrics()
        metrics.set("made", made.isEmpty ? "-" : made.joined(separator: ","))
        metrics.set("new_m2v_needs", asked)
        return .judged(
            [("no texture of a rotation over no surface", made.isEmpty), ("no m2v need asked", asked == 0)], metrics,
            proof: gpu.proof)
    }
}
