import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// RGB10A8_2P_XR10 (550) 32x8, usage ShaderRead, over plane 0 of 'b3a8' surfaces whose planes are not a 4-byte and a
/// 1-byte one (4+2, 8+1 and 4+4 bytes a texel, rows of their elements, plane 1 right after plane 0, every byte
/// i * 37 + 11), each texel read by a kernel as floats. An M4 Max (m2v-textures5 probes) reads every texel by the
/// two-plane rule whatever the planes' elements: red, green, blue (code - 384) / 510 of plane 0's word at 4x (bits
/// 20-29, 10-19, 0-9) and alpha plane 1's byte at x / 255, within 0.002, texel 0 1.0765 -0.1020 -0.7314 0.0431.
/// words_<b0>_<b1>_matched: texels read so (256 each); words_<b0>_<b1>_failed: why one case did not run.
struct XrTwoPlanesAnyElementsTest: HWTest {
    let id = "gpu.gm20b.xr_two_planes_any_elements"
    let name = "RGB10A8_2P_XR10 over b3a8 planes of 4+2, 8+1 and 4+4 bytes reads by the two-plane rule, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 32, height = 8
    static let planes = [(4, 2), (8, 1), (4, 4)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (b0, b1) in Self.planes {
            let key = "words_\(b0)_\(b1)"
            do {
                let matched = try await Self.matched(gpu, b0, b1)
                metrics.set(key + "_matched", matched)
                checks.append(("\(b0)+\(b1): every texel by the two-plane rule", matched == Self.width * Self.height))
            } catch {
                metrics.set(key + "_failed", GPU.describe(error))
                checks.append(("\(b0)+\(b1): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func surface(_ b0: Int, _ b1: Int) throws -> IOSurfaceRef {
        let (p0, p1) = (width * b0, width * b1)
        let plane = { (bytes: Int, pitch: Int, offset: Int) -> [CFString: Any] in
            [
                kIOSurfacePlaneWidth: width, kIOSurfacePlaneHeight: height, kIOSurfacePlaneBytesPerElement: bytes,
                kIOSurfacePlaneBytesPerRow: pitch, kIOSurfacePlaneOffset: offset, kIOSurfacePlaneSize: pitch * height,
            ]
        }
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: width, kIOSurfaceHeight: height, kIOSurfacePixelFormat: OSType(0x6233_6138),
            kIOSurfaceAllocSize: (p0 + p1) * height,
            kIOSurfacePlaneInfo: [plane(b0, p0, 0), plane(b1, p1, p0 * height)],
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no b3a8 surface") }
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< IOSurfaceGetAllocSize(surface) { base[i] = UInt8(truncatingIfNeeded: i * 37 + 11) }
        IOSurfaceUnlock(surface, [], nil)
        return surface
    }

    /// Texels whose sample is the two-plane rule's value of the surface's bytes
    static func matched(_ gpu: GPU, _ b0: Int, _ b1: Int) async throws -> Int {
        let surface = try surface(b0, b1)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: XRTwoPlaneTest.format, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no 550 texture over the \(b0)+\(b1) surface")
        }
        let texels = try await gpu.texels(texture)
        IOSurfaceLock(surface, .readOnly, nil)
        defer { IOSurfaceUnlock(surface, .readOnly, nil) }
        let words = IOSurfaceGetBaseAddressOfPlane(surface, 0), alphas = IOSurfaceGetBaseAddressOfPlane(surface, 1)
        let (p0, p1) = (IOSurfaceGetBytesPerRowOfPlane(surface, 0), IOSurfaceGetBytesPerRowOfPlane(surface, 1))
        return (0 ..< (width * height)).filter { i in
            let (x, y) = (i % width, i / width)
            let word = words.loadUnaligned(fromByteOffset: y * p0 + 4 * x, as: UInt32.self)
            let alpha = alphas.load(fromByteOffset: y * p1 + x, as: UInt8.self)
            let want = XRTwoPlaneModel.values(UInt64(alpha) << 32 | UInt64(word))
            return (0 ..< 4).allSatisfy { abs(Double(texels[i][$0]) - want[$0]) < 0.002 }
        }.count
    }
}
