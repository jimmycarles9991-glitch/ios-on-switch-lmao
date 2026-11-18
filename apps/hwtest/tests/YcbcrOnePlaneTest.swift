import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// One-plane 4:2:2 YCbCr textures (YCBCR8_422_1P 501, its sRGB 521) over '2vuy', 'yuvs' and 'yuvf' surfaces, which went
/// to m2v (tex.shape term 6). An M4 Max reads the plane's bytes as Y0 Cb Y1 Cr whatever the surface's order, as the
/// two-plane 4:2:2 format (502, 522) reads those codes split into a '422v' or '422f' surface (raw/test/ycbcroracle.mm);
/// the raw path splits the plane with the copy engine and runs the 4:2:2 conversion. Each arm reads both textures through
/// a draw: off_<format>_<surface> the texels that differ (0 to pass), paths raw/N (before: the one-plane texture m2v's).
struct YcbcrOnePlaneTest: HWTest {
    let id = "gpu.raw.ycbcr_one_plane"
    let name = "One-plane 4:2:2 YCbCr textures read as Metal reads them, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Arm {
        let format: UInt
        let surface: String
        let one: OSType
        let two: OSType
    }

    static let arms = [
        Arm(
            format: 501, surface: "2vuy", one: kCVPixelFormatType_422YpCbCr8,
            two: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange),
        Arm(
            format: 501, surface: "yuvs", one: kCVPixelFormatType_422YpCbCr8_yuvs,
            two: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange),
        Arm(
            format: 521, surface: "yuvf", one: kCVPixelFormatType_422YpCbCr8FullRange,
            two: kCVPixelFormatType_422YpCbCr8BiPlanarFullRange),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for arm in Self.arms {
            let key = "\(arm.format)_\(arm.surface)"
            let off = try await armOff(gpu, arm)
            metrics.set("off_" + key, off.map(String.init) ?? "no texture")
            checks.append(("\(key): read as its codes split into two planes", off == 0))
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(("every command buffer raw", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    func armOff(_ gpu: GPU, _ arm: Arm) async throws -> Int? {
        let (w, h) = (34, 9)
        guard let oneBuffer = YcbcrCornerTest.pixelBuffer(w, h, arm.one),
            let one = CVPixelBufferGetIOSurface(oneBuffer)?.takeUnretainedValue(),
            let twoBuffer = YcbcrCornerTest.pixelBuffer(w, h, arm.two),
            let two = CVPixelBufferGetIOSurface(twoBuffer)?.takeUnretainedValue()
        else { return nil }
        IOSurfaceLock(one, [], nil)
        IOSurfaceLock(two, [], nil)
        let row = IOSurfaceGetBytesPerRow(one)
        let bytes = IOSurfaceGetBaseAddress(one).bindMemory(to: UInt8.self, capacity: row * h)
        let y = IOSurfaceGetBaseAddressOfPlane(two, 0).bindMemory(to: UInt8.self, capacity: 1)
        let c = IOSurfaceGetBaseAddressOfPlane(two, 1).bindMemory(to: UInt8.self, capacity: 1)
        let (yRow, cRow) = (IOSurfaceGetBytesPerRowOfPlane(two, 0), IOSurfaceGetBytesPerRowOfPlane(two, 1))
        for r in 0..<h {
            for pair in 0..<(w / 2) {
                let at = r * row + 4 * pair
                for k in 0..<4 { bytes[at + k] = UInt8(truncatingIfNeeded: (at + k) &* 53 &+ r &* 7) }
                y[r * yRow + 2 * pair] = bytes[at]
                y[r * yRow + 2 * pair + 1] = bytes[at + 2]
                c[r * cRow + 2 * pair] = bytes[at + 1]
                c[r * cRow + 2 * pair + 1] = bytes[at + 3]
            }
        }
        IOSurfaceUnlock(two, [], nil)
        IOSurfaceUnlock(one, [], nil)
        let make = { (s: IOSurfaceRef, format: UInt) -> MTLTexture? in
            guard let pixelFormat = MTLPixelFormat(rawValue: format) else { return nil }
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: pixelFormat, width: w, height: h, mipmapped: false)
            d.usage = .shaderRead
            return gpu.device.makeTexture(descriptor: d, iosurface: s, plane: 0)
        }
        guard let a = make(one, arm.format), let b = make(two, arm.format + 1) else { return nil }
        let ta = try await YcbcrP3MatrixTest.drawnTexels(gpu, a), tb = try await YcbcrP3MatrixTest.drawnTexels(gpu, b)
        return zip(ta, tb).filter { $0.0 != $0.1 }.count
    }
}
