import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Subsampled sRGB YCbCr targets under a P3 matrix store as Apple's GPU stores them (m2v-ycbcr-p3 hwtest-spec.txt, the
/// rule harness/p3storeoracle.mm holds an M4 Max to on every code): a stored code is the low 8 or 10 bits of the sRGB
/// encode of the drawn code taken as a linear step of 1/8192, a chroma code the block mean of those, half up. 64x32
/// '420v' P3_D65 as 520, '422f' DCI_P3 as 522, 'x420' P3_D65 as 525 and 'xf22' DCI_P3 as 526, each drawn a 2x2 colour
/// pattern (DontCare, one full-frame strip), then cleared (a 10-bit luma of 726 is a clear held toward zero, not to
/// the nearest half; a pattern luma of 128 or 512 at green 0.5 the plain codes stored, not the encode). Fields:
/// p3_store_codes= codes compared, p3_store_off= codes off, p3_store_max_off=, p3_store_first_off= the first off as
/// format_plane_x_y_got_want, paths=, failed_cbs=.
struct P3YcbcrStoreSubsampledTest: HWTest {
    let id = "gpu.gm20b.p3_ycbcr_store_subsampled"
    let name = "Subsampled sRGB YCbCr targets under P3 matrices store Metal's codes, drawn and cleared, on raw"
    let subsystem = Subsystem.metal

    struct Target {
        let format: UInt
        let fourcc: OSType
        let matrix: String
        let tenBit: Bool
        let down: Bool
        /// Luma of the pattern's (0,0), (1,0), (0,1), (1,1) texels
        let luma: [Int]
        let chromaEven: (Int, Int)
        let chromaOdd: (Int, Int)
        /// The clear's luma, Cb and Cr
        let cleared: (Int, Int, Int)
    }

    static let luma8 = [54, 48, 0, 76], luma10 = [224, 109, 0, 724]
    static let targets = [
        Target(
            format: 520, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, matrix: "P3_D65", tenBit: false,
            down: true, luma: luma8, chromaEven: (77, 72), chromaOdd: (77, 72), cleared: (17, 171, 184)
        ),
        Target(
            format: 522, fourcc: kCVPixelFormatType_422YpCbCr8BiPlanarFullRange, matrix: "DCI_P3", tenBit: false,
            down: false, luma: luma8, chromaEven: (130, 24), chromaOdd: (24, 119), cleared: (17, 171, 184)
        ),
        Target(
            format: 525, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, matrix: "P3_D65", tenBit: true,
            down: true, luma: luma10, chromaEven: (349, 224), chromaOdd: (349, 224), cleared: (728, 438, 564)
        ),
        Target(
            format: 526, fourcc: kCVPixelFormatType_422YpCbCr10BiPlanarFullRange, matrix: "DCI_P3", tenBit: true,
            down: false, luma: luma10, chromaEven: (644, 55), chromaOdd: (55, 394), cleared: (728, 438, 564)
        ),
    ]
    static let width = 64, height = 32
    static let clear = MTLClearColor(red: 0.25, green: 0.30066001415252686, blue: 0.6, alpha: 1)

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct P3Out { float4 position [[position]]; };
    vertex P3Out p3_store_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]]) {
        return {corners[vid]};
    }
    fragment float4 p3_store_pattern(P3Out in [[stage_in]]) {
        const float4 colours[4] = {float4(1.0, 0.5, 0.25, 1), float4(0.0, 1.0, 0.75, 1), float4(0.5, 0.0, 1.0, 1),
                                   float4(0.25, 0.75, 0.0, 1)};
        const uint2 q = uint2(in.position.xy) & 1;
        return colours[q.y * 2 + q.x];
    }
    """

    struct Tally {
        var codes = 0
        var off = 0
        var maxOff = 0
        var first = "-"
        var paths: [String] = []

        mutating func add(_ format: UInt, _ plane: Int, _ x: Int, _ y: Int, got: Int, want: Int) {
            codes += 1
            guard got != want else { return }
            off += 1
            maxOff = max(maxOff, abs(got - want))
            if first == "-" { first = "\(format)_\(plane)_\(x)_\(y)_\(got)_\(want)" }
        }
    }

    /// Each target drawn the pattern, then cleared, every code of both planes compared
    static func exercise(_ gpu: GPU, _ library: MTLLibrary, _ tally: inout Tally) async throws {
        let (w, h) = (width, height)
        for t in targets {
            var made: CVPixelBuffer?
            let attributes =
                [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
            guard CVPixelBufferCreate(nil, w, h, t.fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made,
                  let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
                  let format = MTLPixelFormat(rawValue: t.format)
            else { throw TestFailure("no \(t.format) surface") }
            IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, t.matrix as CFString)
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: w, height: h, mipmapped: false
            )
            descriptor.usage = [.renderTarget, .shaderRead]
            guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
                throw TestFailure("no \(t.format) texture")
            }
            let state = try await gpu.device.makeRenderPipelineState(
                descriptor: {
                    let d = MTLRenderPipelineDescriptor()
                    d.vertexFunction = library.makeFunction(name: "p3_store_vertex")
                    d.fragmentFunction = library.makeFunction(name: "p3_store_pattern")
                    d.colorAttachments[0].pixelFormat = format
                    return d
                }()
            )
            for drawn in [true, false] {
                tally.paths.append(
                    try await gpu.run(tolerateError: true) { commands in
                        let pass = MTLRenderPassDescriptor()
                        pass.colorAttachments[0].texture = texture
                        pass.colorAttachments[0].loadAction = drawn ? .dontCare : .clear
                        pass.colorAttachments[0].clearColor = clear
                        pass.colorAttachments[0].storeAction = .store
                        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                        if drawn {
                            encoder.setRenderPipelineState(state)
                            var corners = GPU.fullScreen
                            encoder.setVertexBytes(
                                &corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0
                            )
                            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                        }
                        encoder.endEncoding()
                    }.path
                )
                compare(buffer, t, drawn: drawn, &tally)
            }
        }
    }

    /// Both planes' codes against the target's: luma per texel, a (Cb, Cr) pair per chroma texel
    static func compare(_ buffer: CVPixelBuffer, _ t: Target, drawn: Bool, _ tally: inout Tally) {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        func code(_ plane: Int, _ x: Int, _ y: Int) -> Int {
            let row =
                CVPixelBufferGetBaseAddressOfPlane(buffer, plane)!
                    + y * CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            let bytes = row.assumingMemoryBound(to: UInt8.self)
            if !t.tenBit { return Int(bytes[x]) }
            let word = Int(bytes[2 * x]) | Int(bytes[2 * x + 1]) << 8
            return word & 63 == 0 ? word >> 6 : -word
        }
        for y in 0 ..< height {
            for x in 0 ..< width {
                let want = drawn ? t.luma[(y & 1) * 2 + (x & 1)] : t.cleared.0
                tally.add(t.format, 0, x, y, got: code(0, x, y), want: want)
            }
        }
        for y in 0 ..< (t.down ? height / 2 : height) {
            let pair = drawn ? (y & 1 == 0 ? t.chromaEven : t.chromaOdd) : (t.cleared.1, t.cleared.2)
            for x in 0 ..< width / 2 {
                tally.add(t.format, 1, 2 * x, y, got: code(1, 2 * x, y), want: pair.0)
                tally.add(t.format, 1, 2 * x + 1, y, got: code(1, 2 * x + 1, y), want: pair.1)
            }
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        let library = try await gpu.device.makeLibrary(source: Self.source, options: nil)
        var tally = Tally()
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            try await Self.exercise(gpu, library, &tally)
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("p3_store_codes", tally.codes)
        metrics.set("p3_store_off", tally.off)
        metrics.set("p3_store_max_off", tally.maxOff)
        metrics.set("p3_store_first_off", tally.first)
        metrics.set("paths", tally.paths.isEmpty ? "-" : Set(tally.paths).sorted().joined(separator: ","))
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every code Metal's", tally.codes > 0 && tally.off == 0))
        checks.append(("every pass on the driver", !tally.paths.isEmpty && tally.paths.allSatisfy { $0 == "gm20b" }))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
