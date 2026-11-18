import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Two-plane YCbCr textures whose reads run past their planes' rows, which Metal reads from the bytes there up to the end
/// of the allocation's last page (an M4 Max, raw/test/ycbcroracle.mm): taller than the surface (540 16x12 of 16x8 '420v'),
/// a format's chroma finer than the surface's (541 over '420v'), plane 1 of a 4:2:0 surface read as luma (540 16x16), and
/// rows of more codes than the plane's pitch holds (540 80x8 of 16x8: each row runs on into the next, which the copy engine
/// gathers into the planes' pool images on the raw path). Each arm reads a second texture of the same format over a surface whose planes hold those bytes in their own rows,
/// a form the raw path took before: off_<arm> counts the texels that differ (0 to pass); paths raw/N (before the change
/// the first texture of each arm was an m2v twin: tex.shape term 6).
struct YcbcrPastRowsTest: HWTest {
    let id = "gpu.raw.ycbcr_past_rows"
    let name = "YCbCr textures read past their planes' rows as Metal reads them, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Arm {
        let name: String
        let format: UInt
        let plane: Int
        let width: Int, height: Int
        let surfaceWidth: Int, surfaceHeight: Int
        let reference: OSType
    }

    static let arms = [
        Arm(
            name: "taller", format: 540, plane: 0, width: 16, height: 12, surfaceWidth: 16, surfaceHeight: 8,
            reference: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        Arm(
            name: "finer", format: 541, plane: 0, width: 16, height: 8, surfaceWidth: 16, surfaceHeight: 8,
            reference: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange),
        Arm(
            name: "plane1", format: 540, plane: 1, width: 16, height: 16, surfaceWidth: 16, surfaceHeight: 8,
            reference: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange),
        Arm(
            name: "wider", format: 540, plane: 0, width: 80, height: 8, surfaceWidth: 16, surfaceHeight: 8,
            reference: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for arm in Self.arms {
            let off = try await armOff(gpu, arm)
            metrics.set("off_" + arm.name, off.map(String.init) ?? "no texture")
            checks.append(("\(arm.name): read as the same bytes in a surface's own rows", off == 0))
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(("every command buffer raw", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The rows each plane's read takes, as (offset, pitch, row bytes, rows) of the read surface
    func reads(_ arm: Arm, _ s: IOSurfaceRef) -> [(offset: Int, pitch: Int, bytes: Int, rows: Int)] {
        let base = Int(bitPattern: IOSurfaceGetBaseAddress(s))
        let at = { (plane: Int) in Int(bitPattern: IOSurfaceGetBaseAddressOfPlane(s, plane)) - base }
        let pitch = { (plane: Int) in IOSurfaceGetBytesPerRowOfPlane(s, plane) }
        if arm.plane == 1 { return [(at(1), pitch(1), arm.width, arm.height)] }
        let chromaRows = arm.format == 541 ? arm.height : (arm.height + 1) / 2
        return [(at(0), pitch(0), arm.width, arm.height), (at(1), pitch(1), (arm.width + 1) / 2 * 2, chromaRows)]
    }

    func armOff(_ gpu: GPU, _ arm: Arm) async throws -> Int? {
        guard
            let readBuffer = YcbcrCornerTest.pixelBuffer(
                arm.surfaceWidth, arm.surfaceHeight, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            let read = CVPixelBufferGetIOSurface(readBuffer)?.takeUnretainedValue(),
            let referenceBuffer = YcbcrCornerTest.pixelBuffer(arm.width, arm.height, arm.reference),
            let reference = CVPixelBufferGetIOSurface(referenceBuffer)?.takeUnretainedValue()
        else { return nil }
        let page = Int(vm_page_size)
        let mapped = (IOSurfaceGetAllocSize(read) + page - 1) / page * page
        let rows = reads(arm, read)
        guard rows.allSatisfy({ $0.offset + $0.pitch * ($0.rows - 1) + $0.bytes <= mapped }) else { return nil }
        IOSurfaceLock(read, [], nil)
        let bytes = IOSurfaceGetBaseAddress(read).bindMemory(to: UInt8.self, capacity: mapped)
        for i in 0..<IOSurfaceGetAllocSize(read) { bytes[i] = UInt8(truncatingIfNeeded: i &* 37 &+ i >> 7) }
        IOSurfaceLock(reference, [], nil)
        let referencePlanes = arm.plane == 1 ? [1] : [0, 1]
        for (k, plane) in referencePlanes.enumerated() {
            let r = rows[k]
            let to = IOSurfaceGetBaseAddressOfPlane(reference, plane).bindMemory(to: UInt8.self, capacity: 1)
            let toPitch = IOSurfaceGetBytesPerRowOfPlane(reference, plane)
            for y in 0..<min(r.rows, IOSurfaceGetHeightOfPlane(reference, plane)) {
                memcpy(to + y * toPitch, bytes + r.offset + y * r.pitch, r.bytes)
            }
        }
        IOSurfaceUnlock(reference, [], nil)
        IOSurfaceUnlock(read, [], nil)
        let make = { (s: IOSurfaceRef) -> MTLTexture? in
            guard let format = MTLPixelFormat(rawValue: arm.format) else { return nil }
            let d = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: arm.width, height: arm.height, mipmapped: false)
            d.usage = .shaderRead
            return gpu.device.makeTexture(descriptor: d, iosurface: s, plane: arm.plane)
        }
        guard let past = make(read), let own = make(reference) else { return nil }
        let a = try await YcbcrP3MatrixTest.drawnTexels(gpu, past),
            b = try await YcbcrP3MatrixTest.drawnTexels(gpu, own)
        return zip(a, b).filter { $0.0 != $0.1 }.count
    }
}
