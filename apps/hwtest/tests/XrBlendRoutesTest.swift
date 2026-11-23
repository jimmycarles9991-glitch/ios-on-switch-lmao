import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Extended-range targets under overlapping premultiplied source-over draws, each route the driver can draw them by
/// (`routes`; today stand_in: the pass renders into an RGBA16Float stand-in the ROP blends and stores its codes,
/// RGB10A8_2P_XR10 550 into its two planes over a 'b3a8' surface, BGRA10_XR 552 into its code words over a 'w40a'
/// surface), against Apple's codes. Golden scene: per format, 4 bands of rows (in range; overbright and past XR's
/// range; NaN in a colour or in alpha; opaque, clear and near-range mixes) each with 8 quads in one pass, quad k
/// covering the band from column 16k on, so column band j blends quads 0 to j over the clear: 32 regions, each one
/// texel of XrBlendRoutesGoldens (an M4 Max, this scene). Per format and route: <format>_<route>_worst_code, the
/// largest difference of a channel's code from Apple's over every texel (alpha: 550's byte, 552's code);
/// _band<b>_worst, the same within band b; <format>_<route>_off, texels not Apple's; xr_<route>_worst_code, both
/// formats' largest. Timing scene: the 8 staircase quads of band 0 over a 1280x720 surface of each format, one pass;
/// <format>_<route>_us, its GPU time, and us_<route> both formats' sum. Reported, not judged: the ROP blends in half
/// rounding toward zero where Apple's rounds to nearest, and reads a sum past XR's range unclamped where Apple's tile
/// read clamps it (band 1). Judged: a route ran, every command buffer on the driver, no command buffer failed. A cell
/// whose command buffer fails or whose target cannot be made reports <format>_<route>_failed.
struct XrBlendRoutesTest: HWTest {
    let id = "gpu.gm20b.xr_blend_routes"
    let name = "Overlapping source-over draws into RGB10A8_2P_XR10 and BGRA10_XR surfaces per route, against Apple"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Format {
        let name: String
        let pixel: MTLPixelFormat
        let surface: OSType
        /// A texel's red, green, blue and alpha codes
        let codes: (UInt64) -> [Int]
        let golden: [UInt64]
    }

    struct Target {
        let texture: MTLTexture
        let texels: () -> [UInt64]
    }

    struct Route {
        let name: String
        let make: (GPU, Format, Int, Int) throws -> Target
    }

    static let formats = [
        Format(
            name: "xr550", pixel: MTLPixelFormat(rawValue: 550)!, surface: kCVPixelFormatType_30RGBLE_8A_BiPlanar,
            codes: { t in [20, 10, 0].map { Int(t >> $0 & 0x3ff) } + [Int(t >> 32 & 0xff)] },
            golden: XrBlendRoutesGoldens.xr550
        ),
        Format(
            name: "xr552", pixel: .bgra10_xr, surface: kCVPixelFormatType_40ARGBLEWideGamut,
            codes: { t in [2, 1, 0, 3].map { Int(t >> (16 * $0 + 6) & 0x3ff) } },
            golden: XrBlendRoutesGoldens.xr552
        ),
    ]

    static let routes = [Route(name: "stand_in", make: surfaceTarget)]

    static let band = 16, quads = 8, bands = 4
    static let clear = MTLClearColor(red: 0.1, green: 0.6, blue: -0.2, alpha: 0.5)
    static let colours: [[SIMD4<Float>]] = [
        (0 ..< quads).map { k in
            let a = Float(k + 1) / 10
            return SIMD4(0.9 * a, 0.35 * a, Float(k % 3) * 0.3 * a, a)
        },
        [
            SIMD4(1.2, -0.5, 0.6, 0.8), SIMD4(2.0, 0.1, -1.5, 0.5), SIMD4(0.3, 1.24, -0.74, 0.25),
            SIMD4(-3.0, 4.0, 1.0, 0.6), SIMD4(1.25, 1.25, 1.25, 0.9), SIMD4(0.7, -0.7, 0.2, 1.5),
            SIMD4(0.05, 0.9, 1.1, 0.3), SIMD4(-0.6, 0.2, 2.5, 0.7),
        ],
        [
            SIMD4(0.4, 0.2, 0.1, 0.5), SIMD4(.nan, 0.3, 0.2, 0.4), SIMD4(0.2, 0.2, 0.2, 0.2),
            SIMD4(0.1, 0.5, 0.3, .nan), SIMD4(0.3, 0.1, 0.6, 0.6), SIMD4(0.2, .nan, .nan, 0.3),
            SIMD4(0.5, 0.4, 0.3, 0.7), SIMD4(.infinity, 0.1, -.infinity, 0.5),
        ],
        [
            SIMD4(0.2, 0.4, 0.6, 1), SIMD4(0.5, 0.5, 0.5, 0), SIMD4(1.0, -0.25, 0.0, 0.02), SIMD4(-0.75, 1.25, 0.5, 1),
            SIMD4(0.1, 0.1, 0.1, 0.98), SIMD4(0.6, -0.6, 0.6, 0), SIMD4(0.0, 0.0, 0.0, 0.5),
            SIMD4(1.24, -0.74, 1.0, 0.75),
        ],
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var ran = 0
        for route in Self.routes {
            var worst = 0
            var micros = 0.0
            for format in Self.formats {
                let cell = "\(format.name)_\(route.name)"
                do {
                    let (texels, failed) = try await Self.golden(gpu, format, route)
                    guard let texels else {
                        metrics.set(cell + "_failed", failed)
                        continue
                    }
                    let apart = Self.apart(texels, format)
                    worst = max(worst, apart.worst)
                    metrics.set(cell + "_worst_code", apart.worst)
                    metrics.set(cell + "_off", apart.off)
                    for (b, bandWorst) in apart.bands.enumerated() { metrics.set(cell + "_band\(b)_worst", bandWorst) }
                    ran += 1
                    let timing = try await Self.timed(gpu, format, route)
                    guard !timing.path.hasPrefix("error:") else {
                        metrics.set(cell + "_failed", timing.path)
                        continue
                    }
                    micros += timing.gpuMilliseconds * 1000
                    metrics.set(cell + "_us", timing.gpuMilliseconds * 1000, digits: 0)
                } catch {
                    metrics.set(cell + "_failed", GPU.describe(error))
                }
            }
            metrics.set("xr_\(route.name)_worst_code", worst)
            metrics.set("us_\(route.name)", micros, digits: 0)
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        metrics.fitLog(within: 720, keeping: ["paths", "failed_cbs", "xr_stand_in_worst_code", "us_stand_in"])
        return .judged(
            [
                ("a route drew a format", ran > 0), ("every command buffer on the driver", gpu.paths.offRaw.isEmpty),
                FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics),
            ], metrics, proof: gpu.proof
        )
    }

    static func region(_ x: Int, _ y: Int) -> Int { (y / band) * quads + x / band }

    /// Texels not Apple's, and the largest difference of a channel's code from Apple's, over all and per band
    static func apart(_ texels: [UInt64], _ format: Format) -> (off: Int, worst: Int, bands: [Int]) {
        var off = 0
        var bandWorst = [Int](repeating: 0, count: bands)
        for (i, t) in texels.enumerated() {
            let (x, y) = (i % (band * quads), i / (band * quads))
            let want = format.golden[region(x, y)]
            guard t != want else { continue }
            off += 1
            let apart = zip(format.codes(t), format.codes(want)).map { abs($0 - $1) }.max() ?? 0
            bandWorst[y / band] = max(bandWorst[y / band], apart)
        }
        return (off, bandWorst.max() ?? 0, bandWorst)
    }

    /// The stand-in route: the format's surface, its texture over plane 0, read back through a CPU lock (550: the alpha
    /// byte above the word)
    static func surfaceTarget(_ gpu: GPU, _ format: Format, _ width: Int, _ height: Int) throws -> Target {
        var made: CVPixelBuffer?
        let attributes: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary]
        guard
            CVPixelBufferCreate(nil, width, height, format.surface, attributes as CFDictionary, &made)
            == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue()
        else { throw TestFailure("no \(format.name) pixel buffer") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format.pixel, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no \(format.name) texture over its surface")
        }
        if CVPixelBufferIsPlanar(buffer) {
            return Target(texture: texture) { XRTwoPlaneTest.planes(buffer, width: width, height: height) }
        }
        return Target(texture: texture) {
            CVPixelBufferLockBaseAddress(buffer, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
            let base = CVPixelBufferGetBaseAddress(buffer)!, row = CVPixelBufferGetBytesPerRow(buffer)
            return (0 ..< (width * height)).map { i in
                base.loadUnaligned(fromByteOffset: (i / width) * row + (i % width) * 8, as: UInt64.self)
            }
        }
    }

    static func pass(
        _ gpu: GPU, _ target: Target, bands: Int, width: Int, height: Int
    ) async throws -> GPU.Timing {
        let state = try gpu.render(fragment: "solid_fragment", format: target.texture.pixelFormat) {
            let a = $0.colorAttachments[0]!
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .one
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let step = width / quads, rows = height / bands
        return try await gpu.run(tolerateError: true) { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target.texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            for b in 0 ..< bands {
                for k in 0 ..< quads {
                    let corners = GPU.pixelQuad(k * step, b * rows, width, (b + 1) * rows, width: width, height: height)
                    GPU.draw(encoder, corners: corners, color: colours[b][k])
                }
            }
            encoder.endEncoding()
        }
    }

    /// The golden scene's texels, or why its command buffer failed
    static func golden(_ gpu: GPU, _ format: Format, _ route: Route) async throws -> ([UInt64]?, String) {
        let (width, height) = (band * quads, band * bands)
        let target = try route.make(gpu, format, width, height)
        let timing = try await pass(gpu, target, bands: bands, width: width, height: height)
        return timing.path.hasPrefix("error:") ? (nil, timing.path) : (target.texels(), "")
    }

    static func timed(_ gpu: GPU, _ format: Format, _ route: Route) async throws -> GPU.Timing {
        let target = try route.make(gpu, format, 1280, 720)
        return try await pass(gpu, target, bands: 1, width: 1280, height: 720)
    }
}
