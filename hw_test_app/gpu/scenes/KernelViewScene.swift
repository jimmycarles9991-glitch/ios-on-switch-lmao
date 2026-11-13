import Foundation
import HWTestCore
import Metal

/// Kernels that write half-float textures (RGBA16Float, R16Float), as Core Image's do in PosterBoard and
/// iconservicesagent. m2v writes each through its unsigned integer twin; the raw facade binds a texture header of
/// that format over the same memory (drivers/TegraGM20BMetal/raw/runtime/raw_view.h). Boot 17 sent 216 of
/// PosterBoard's 222 command buffers to m2v for it (compute.view). A sampled source in the same kernel keeps its own
/// header, a read_write texture is read and written through the view, and a bgra8 write is the control. Every value
/// is exact in half, so the bytes must match.
struct KernelViewScene: GPUScene {
    let id = "gpu.raw.kernel_view"
    let name = "Kernels writing half-float textures through a view of another format"

    static let width = 32
    static let height = 16

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let (w, h) = (Self.width, Self.height)
        let texels = w * h
        // multiples of 1/16 below 2: doubled, halved and offset by a coordinate below 32, each stays exact in half
        let start = (0..<texels * 4).map { Float16(Float(($0 * 7) % 32) / 16) }
        let source = try gpu.texture(.rgba16Float, w, h, usage: [.shaderRead])
        let rgba = try gpu.texture(.rgba16Float, w, h, usage: [.shaderWrite])
        let red = try gpu.texture(.r16Float, w, h, usage: [.shaderWrite])
        let both = try gpu.texture(.rgba16Float, w, h, usage: [.shaderRead, .shaderWrite])
        let bgra = try gpu.texture(.bgra8Unorm, w, h, usage: [.shaderWrite])
        try await gpu.upload(Self.bytes(start), to: source, bytesPerRow: w * 8)
        try await gpu.upload(Self.bytes(start), to: both, bytesPerRow: w * 8)
        let kernels: [(String, [MTLTexture])] = [
            ("view_half_write", [source, rgba, red]), ("view_half_read_write", [both]), ("view_bgra_write", [bgra]),
        ]
        var paths: [String] = []
        for (name, textures) in kernels {
            let pipeline = try gpu.compute(name)
            let timing = try await gpu.run { commands in
                let encoder = commands.makeComputeCommandEncoder()!
                encoder.setComputePipelineState(pipeline)
                for (index, texture) in textures.enumerated() { encoder.setTexture(texture, index: index) }
                encoder.dispatchThreadgroups(
                    MTLSize(width: w / 8, height: h / 8, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1))
                encoder.endEncoding()
            }
            paths.append(timing.path)
        }
        var wantRGBA: [Float16] = [], wantRed: [Float16] = [], wantBoth: [Float16] = [], wantBGRA: [UInt8] = []
        for y in 0..<h {
            for x in 0..<w {
                let s = Array(start[(y * w + x) * 4..<(y * w + x) * 4 + 4])
                wantRGBA += [s[0] * 2 + Float16(x), s[1] * 2 + Float16(y), s[2] * 2 + 0.25, s[3] * 2 - 1]
                wantRed.append(Float16(x + y) * 0.5)
                wantBoth += [s[0] * 0.5 + 0.25, s[1] * 0.5 - 0.5, s[2] * 0.5 + Float16(x), s[3] * 0.5 + 1]
                wantBGRA += [UInt8(x + y), UInt8(y), UInt8(x), UInt8(255 - x)]
            }
        }
        let checks: [(String, MTLTexture, Int, [UInt8])] = [
            ("RGBA16Float written", rgba, 8, Self.bytes(wantRGBA)), ("R16Float written", red, 2, Self.bytes(wantRed)),
            ("RGBA16Float read and written", both, 8, Self.bytes(wantBoth)), ("BGRA8Unorm written", bgra, 4, wantBGRA),
        ]
        for (label, texture, bytesPerTexel, want) in checks {
            let got = try await gpu.read(texture, bytesPerPixel: bytesPerTexel)
            let wrong = (0..<texels).filter {
                got[$0 * bytesPerTexel..<($0 + 1) * bytesPerTexel]
                    != want[$0 * bytesPerTexel..<($0 + 1) * bytesPerTexel]
            }
            result.metrics.set(label.lowercased().replacingOccurrences(of: " ", with: "_") + "_wrong", wrong.count)
            result.expect(wrong.isEmpty, "\(label): \(wrong.count) texels wrong, the first at \(wrong.first ?? -1)")
        }
        result.metrics.set("paths", paths.joined(separator: ","))
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(paths.allSatisfy { $0 == "raw" }, "the kernels took \(paths), not raw")
        }
        return result
    }

    private static func bytes(_ halves: [Float16]) -> [UInt8] {
        halves.flatMap { [UInt8($0.bitPattern & 0xff), UInt8($0.bitPattern >> 8)] }
    }
}
