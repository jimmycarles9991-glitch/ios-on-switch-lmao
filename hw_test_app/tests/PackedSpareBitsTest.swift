import CoreImage
import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal
import VideoToolbox

/// Whether anything on the device reads the spare bits of a packed 10-bit surface's words (bits 30-31 and a row's slots
/// past its last code), which the raw path's packed store states write 0. Each surface ('p420', 'pf20', 'p422', 'p444',
/// 98 wide: rows with spare slots) is read with the spare bits clear and then set, by Metal's R10Unorm_PACKED,
/// RG10Unorm_PACKED, RGB10_*_2P_PACKED and YCBCR10_*_2P_PACKED textures, VTPixelTransferSession into its 16-bit twin and
/// Core Image. moved_<reader>_ counts the reads the bits changed: any but 0 names a reader the store must write for. On
/// macOS 27.2.1 none moves (raw/test/packedreaders.mm).
struct PackedSpareBitsTest: HWTest {
    let id = "gpu.packed_spare_bits"
    let name = "Readers of packed 10-bit surfaces' spare bits"
    let subsystem = Subsystem.metal

    struct Surface {
        let fourcc: OSType, twin: OSType, rgb10: UInt, ycbcr10: UInt
    }

    static let surfaces = [
        Surface(fourcc: YcbcrPackedTargetTest.p420, twin: YcbcrPackedTargetTest.x420, rgb10: 546, ycbcr10: 508),
        Surface(fourcc: YcbcrPackedTargetTest.pf20, twin: YcbcrPackedTargetTest.xf20, rgb10: 546, ycbcr10: 508),
        Surface(
            fourcc: YcbcrPackedTargetTest.p422, twin: kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange, rgb10: 547,
            ycbcr10: 509),
        Surface(fourcc: YcbcrPackedTargetTest.p444, twin: YcbcrPackedTargetTest.x444, rgb10: 548, ycbcr10: 510),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let reader = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        var session: VTPixelTransferSession?
        VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session)
        guard let session else { return .fail("no VTPixelTransferSession", Metrics()) }
        defer { VTPixelTransferSessionInvalidate(session) }
        let images = CIContext(options: [.workingFormat: CIFormat.RGBAh])
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for surface in Self.surfaces {
            guard let buffer = YcbcrPackedTargetTest.surface(gpu, surface.fourcc, 588, 98, 8)?.0 else {
                checks.append(("\(surface.fourcc): a surface", false))
                continue
            }
            func reads(_ spare: Bool) async throws -> [String: [UInt8]] {
                Self.fill(buffer, spare: spare)
                var out: [String: [UInt8]] = [:]
                for (name, format, plane) in [
                    ("r10", 588, 0), ("rg10", 589, 1), ("rgb10", surface.rgb10, 0), ("ycbcr10", surface.ycbcr10, 0),
                ] {
                    out["metal_" + name] = try await Self.metal(gpu, reader, buffer, UInt(format), plane)
                }
                out["vt"] = Self.transferred(session, buffer, surface.twin)
                out["ci"] = Self.rendered(images, buffer)
                return out
            }
            let clear = try await reads(false), set = try await reads(true)
            let key =
                String(bytes: withUnsafeBytes(of: surface.fourcc.bigEndian) { Array($0) }, encoding: .ascii) ?? "?"
            for (reader, bytes) in clear.sorted(by: { $0.key < $1.key }) {
                let moved = bytes != set[reader]
                metrics.set("moved_\(reader)_\(key)", moved ? 1 : 0)
                checks.append(("'\(key)' \(reader): read alike with the spare bits set", !bytes.isEmpty && !moved))
            }
        }
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// Random codes in every word of both planes, the spare bits all clear or all set.
    static func fill(_ buffer: CVPixelBuffer, spare: Bool) {
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        var state: UInt64 = 588
        for plane in 0..<2 {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
            let codes = (plane + 1) * CVPixelBufferGetWidthOfPlane(buffer, plane),
                pitch =
                    CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            for y in 0..<CVPixelBufferGetHeightOfPlane(buffer, plane) {
                for x in 0..<((codes + 2) / 3) {
                    var word: UInt32 = spare ? 3 << 30 : 0
                    for i in 0..<3 {
                        state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
                        if 3 * x + i < codes || spare { word |= UInt32(state >> 54) << (10 * i) }
                    }
                    base.storeBytes(of: word, toByteOffset: y * pitch + 4 * x, as: UInt32.self)
                }
            }
        }
    }

    static func metal(
        _ gpu: GPU, _ reader: MTLRenderPipelineState, _ buffer: CVPixelBuffer, _ format: UInt, _ plane: Int
    )
        async throws -> [UInt8]
    {
        guard let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let pixelFormat = MTLPixelFormat(rawValue: format)
        else { return [] }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat, width: CVPixelBufferGetWidthOfPlane(buffer, plane),
            height: CVPixelBufferGetHeightOfPlane(buffer, plane), mipmapped: false)
        descriptor.usage = [.shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: plane) else {
            return []
        }
        let texels = try await YcbcrPackedTargetTest.read(gpu, reader, texture)
        return texels.withUnsafeBytes { Array($0) }
    }

    static func transferred(_ session: VTPixelTransferSession, _ buffer: CVPixelBuffer, _ twin: OSType) -> [UInt8] {
        var made: CVPixelBuffer?
        let attributes = [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary
        guard
            CVPixelBufferCreate(
                nil, CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer), twin, attributes, &made)
                == kCVReturnSuccess, let to = made,
            VTPixelTransferSessionTransferImage(session, from: buffer, to: to) == noErr
        else { return [] }
        CVPixelBufferLockBaseAddress(to, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(to, .readOnly) }
        var out: [UInt8] = []
        for plane in 0..<2 {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(to, plane) else { return [] }
            let bytes = CVPixelBufferGetBytesPerRowOfPlane(to, plane) * CVPixelBufferGetHeightOfPlane(to, plane)
            out += UnsafeRawBufferPointer(start: base, count: bytes)
        }
        return out
    }

    static func rendered(_ context: CIContext, _ buffer: CVPixelBuffer) -> [UInt8] {
        let (w, h) = (CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer))
        var out = [UInt8](repeating: 0, count: 8 * w * h)
        context.render(
            CIImage(cvPixelBuffer: buffer), toBitmap: &out, rowBytes: 8 * w,
            bounds: CGRect(x: 0, y: 0, width: w, height: h),
            format: .RGBAh, colorSpace: nil)
        return out
    }
}
