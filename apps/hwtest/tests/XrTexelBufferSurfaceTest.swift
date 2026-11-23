import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Extended-range texture buffers over an IOSurface: a 4096x1 surface of 4-byte elements, every word 0xe7f80100 (codes
/// red 639, green 512, blue 256, the two bits above them 3), read texel by texel by a kernel (shaders/HWTest.metal
/// read_texture_buffer) through a 4096-texel BGR10_XR (554) texture buffer and through one of format 550 over the same
/// surface. An M4 Max (m2v-textures4 probes/tb.mm, and this test's case code on it) reads every texel red 0.5, green
/// 0.250980, blue -0.250980 (the codes' (code - 384) / 510) through both, alpha 1 through 554 and 0 through 550. r= g=
/// b= texel 0's colour through 554, a554= a550= its alpha through each; off= texels of either read off Apple's value by
/// half a code or more (a colour channel; alpha exact). Every command buffer raw, failed_cbs=0.
struct XrTexelBufferSurfaceTest: HWTest {
    let id = "gpu.gm20b.xr_texel_buffer_surface"
    let name = "Extended-range texture buffers over an IOSurface read an M4 Max's values, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let width = 4096
    static let word: UInt32 = 0xe7f8_0100
    static let colour = SIMD3<Float>(0.5, 128.0 / 510, -128.0 / 510)
    static let alphas: [(format: UInt, alpha: Float)] = [(554, 1), (550, 0)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var off = 0
        do {
            let surface = try Self.surface()
            for arm in Self.alphas {
                do {
                    let texels = try await Self.read(gpu, surface, format: arm.format)
                    if arm.format == 554 {
                        metrics.set("r", Double(texels[0].x), digits: 6)
                        metrics.set("g", Double(texels[0].y), digits: 6)
                        metrics.set("b", Double(texels[0].z), digits: 6)
                    }
                    metrics.set("a\(arm.format)", Double(texels[0].w), digits: 6)
                    let wrong = Self.off(texels, alpha: arm.alpha)
                    off += wrong
                    checks.append(("\(arm.format): every texel Apple's value", wrong == 0))
                } catch {
                    checks.append(("\(arm.format): \(GPU.describe(error))", false))
                }
            }
        } catch {
            checks.append(("surface: \(GPU.describe(error))", false))
        }
        metrics.set("off", off)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["r", "g", "b", "a554", "a550", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func off(_ texels: [SIMD4<Float>], alpha: Float) -> Int {
        let halfCode: Float = 0.5 / 510
        return texels.filter { t in
            !(abs(t.x - colour.x) < halfCode && abs(t.y - colour.y) < halfCode && abs(t.z - colour.z) < halfCode
                && t.w == alpha)
        }.count
    }

    static func surface() throws -> IOSurface {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: 1, .bytesPerElement: 4, .bytesPerRow: width * 4, .allocSize: width * 4,
            .pixelFormat: 0x4247_5241,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        guard surface.lock(options: [], seed: nil) == 0 else { throw TestFailure("IOSurfaceLock failed") }
        surface.baseAddress.bindMemory(to: UInt32.self, capacity: width).update(repeating: word, count: width)
        surface.unlock(options: [], seed: nil)
        return surface
    }

    static func read(_ gpu: GPU, _ surface: IOSurface, format: UInt) async throws -> [SIMD4<Float>] {
        let descriptor = MTLTextureDescriptor.textureBufferDescriptor(
            with: MTLPixelFormat(rawValue: format)!, width: width, resourceOptions: [],
            usage: [.shaderRead, .shaderWrite]
        )
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no \(format) texture buffer over the surface")
        }
        let out = try gpu.buffer(width * 16)
        let pipeline = try gpu.compute("read_texture_buffer")
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setTexture(texture, index: 0)
            encoder.setBuffer(out, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: width, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        return Array(
            UnsafeBufferPointer(start: out.contents().bindMemory(to: SIMD4<Float>.self, capacity: width), count: width)
        )
    }
}
