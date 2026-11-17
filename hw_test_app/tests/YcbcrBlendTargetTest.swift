import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Blended and channel-masked states into two-plane YCbCr attachments (RawMetalYcbcrTargets.mm standInTargetState: before,
/// raw:state.ycbcr left them to m2v, which renders into no YCbCr target). A clear and three draws a pass. Masked states,
/// and states blending by the destination alpha (which a format without alpha reads as 1), store Metal's codes exactly;
/// one blending over the source alpha lies within one code of Metal's, which blends in halves of its own rounding
/// (raw/test/ycbcrblendoracle.mm on an M4 Max: the route's codes 0 off for those, at most one off for these).
/// off_<case>_<format> counts codes past that bound (0 to pass), one_<case>_<format> the codes one off (a figure),
/// paths every command buffer's path (raw/N to pass).
struct YcbcrBlendTargetTest: HWTest {
    let id = "gpu.raw.ycbcr_blend"
    let name = "Blended and masked states into two-plane YCbCr targets on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Case {
        let name: String
        let blending: Bool
        let source: MTLBlendFactor
        let destination: MTLBlendFactor
        let mask: MTLColorWriteMask
        let clearAlpha: Double
        let tolerance: Int
    }

    static let cases = [
        Case(
            name: "over", blending: true, source: .sourceAlpha, destination: .oneMinusSourceAlpha, mask: .all,
            clearAlpha: 1, tolerance: 1),
        Case(
            name: "dst_alpha", blending: true, source: .destinationAlpha, destination: .oneMinusDestinationAlpha,
            mask: .all, clearAlpha: 0.25, tolerance: 0),
        Case(
            name: "mask_luma", blending: false, source: .one, destination: .zero, mask: .green, clearAlpha: 1,
            tolerance: 0),
        Case(
            name: "mask_chroma", blending: false, source: .one, destination: .zero, mask: [.red, .blue], clearAlpha: 1,
            tolerance: 0),
    ]

    static let targets = [
        YcbcrTargetTest.Target(format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        YcbcrTargetTest.Target(format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange),
        YcbcrTargetTest.Target(format: 543, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for target in Self.targets {
            for c in Self.cases {
                let key = "\(c.name)_\(target.format)"
                do {
                    let (off, one) = try await exercise(gpu, target, c)
                    metrics.set("off_" + key, off)
                    metrics.set("one_" + key, one)
                    checks.append(("\(key): codes within \(c.tolerance) of Metal's", off == 0))
                } catch {
                    metrics.set("off_" + key, "failed")
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(("every command buffer raw", raw))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// The codes past the case's bound and the codes one off, over both planes.
    func exercise(_ gpu: GPU, _ target: YcbcrTargetTest.Target, _ c: Case) async throws -> (Int, Int) {
        let (w, h) = (YcbcrTargetTest.width, YcbcrTargetTest.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, target.fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: target.format)
        else { throw TestFailure("no surface") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture")
        }
        let state = try gpu.render(fragment: "texel_values", format: format) { d in
            let a = d.colorAttachments[0]!
            a.isBlendingEnabled = c.blending
            a.writeMask = c.mask
            a.sourceRGBBlendFactor = c.source
            a.sourceAlphaBlendFactor = c.source
            a.destinationRGBBlendFactor = c.destination
            a.destinationAlphaBlendFactor = c.destination
        }
        let draws = (0..<3).map { Self.values(w * h, seed: UInt64(1 + $0)) }
        let buffers = try draws.map { try gpu.buffer($0) }
        let clear = MTLClearColor(red: 0.31, green: 0.52, blue: 0.73, alpha: c.clearAlpha)
        YcbcrTargetTest.scribble(buffer, seed: 3)
        var width = UInt32(w)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBytes(&width, length: 4, index: 1)
            for data in buffers {
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
        let held = Self.held(draws, clear: clear, c)
        let want = YcbcrTargetTest.codes(target, held, .drawnFloat)
        return Self.deviations(buffer, want, wide: target.tenBit, tolerance: c.tolerance)
    }

    /// Each texel as the pass holds it in floats over the clear's halves, a channel the mask keeps the clear's, and a
    /// destination alpha of 1.
    static func held(_ draws: [[SIMD4<Float>]], clear: MTLClearColor, _ c: Case) -> [SIMD4<Float>] {
        let start = SIMD4<Float>(
            Float(Float16(clear.red)), Float(Float16(clear.green)), Float(Float16(clear.blue)), 1)
        let writes = [c.mask.contains(.red), c.mask.contains(.green), c.mask.contains(.blue), c.mask.contains(.alpha)]
        return (0..<draws[0].count).map { i in
            var v = start
            for draw in draws {
                let s = draw[i]
                for k in 0..<3 where writes[k] {
                    switch (c.blending, c.source) {
                    case (false, _), (true, .destinationAlpha): v[k] = s[k]
                    default: v[k] = s[k] * s.w + v[k] * (1 - s.w)
                    }
                }
            }
            return v
        }
    }

    /// The codes of `planes` stored more than `tolerance` off, and those exactly one off.
    static func deviations(_ buffer: CVPixelBuffer, _ planes: [[UInt16]], wide: Bool, tolerance: Int) -> (Int, Int) {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        var (past, one) = (0, 0)
        for (plane, codes) in planes.enumerated() {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else {
                past += codes.count
                continue
            }
            let row = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            let perRow = codes.count / rows
            for y in 0..<rows {
                let line = base.advanced(by: y * row)
                for x in 0..<perRow {
                    let stored =
                        wide
                        ? (UInt16(line.load(fromByteOffset: 2 * x, as: UInt8.self))
                            | UInt16(line.load(fromByteOffset: 2 * x + 1, as: UInt8.self)) << 8) >> 6
                        : UInt16(line.load(fromByteOffset: x, as: UInt8.self))
                    let off = abs(Int(stored) - Int(codes[y * perRow + x]))
                    past += off > tolerance ? 1 : 0
                    one += off == 1 ? 1 : 0
                }
            }
        }
        return (past, one)
    }

    /// Uniform floats in [0, 1], alpha too.
    static func values(_ count: Int, seed: UInt64) -> [SIMD4<Float>] {
        var state = seed
        func next() -> Float {
            state = state &* 6_364_136_223_846_793_005 &+ 1_442_695_040_888_963_407
            return Float(state >> 40) / Float(1 << 24)
        }
        return (0..<count).map { _ in SIMD4(next(), next(), next(), next()) }
    }
}
