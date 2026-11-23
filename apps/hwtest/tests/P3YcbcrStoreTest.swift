import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Stores into two-plane 4:4:4 YCbCr targets over surfaces tagged IOSurfaceYCbCrMatrix P3_D65, p3storeoracle.mm's draw
/// (harness): a 64x16 '444v' or 'x444' surface of scribbled bytes, a pass that loads nothing and draws each texel a
/// float of std::mt19937(7)'s k / 4095, Store. The rule is the oracle's, measured on an M4 Max (every code of this
/// scene too): 503 and 507 store their codes, luma round(G * M), Cb round(B * M), Cr round(R * M), M 255 or 1023, each
/// value held as a half first, toward zero at 10 bits (YcbcrStore.codes); 523 stores 128 in every code and 527 384
/// (each 16-bit word 384 << 6), whatever is drawn. Tolerance 0. Per format <fmt>_luma_off and <fmt>_chroma_off codes
/// off, <fmt>_texel0 texel 0's luma/Cb/Cr codes (an M4 Max: 503 76/33/11, 507 305/134/44, 523 128/128/128, 527
/// 384/384/384); every command buffer raw, failed_cbs=0.
struct P3YcbcrStoreTest: HWTest {
    let id = "gpu.gm20b.p3_ycbcr_store"
    let name = "Draws into 4:4:4 YCbCr targets under the P3_D65 matrix store Metal's codes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Case {
        let format: UInt
        let fourcc: OSType
        /// Every code's value whatever is drawn; nil: the codes rule
        let constant: UInt16?

        var tenBit: Bool { fourcc == kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange }
    }

    static let cases = [
        Case(format: 503, fourcc: kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, constant: nil),
        Case(format: 507, fourcc: kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, constant: nil),
        Case(format: 523, fourcc: kCVPixelFormatType_444YpCbCr8BiPlanarVideoRange, constant: 128),
        Case(format: 527, fourcc: kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, constant: 384),
    ]
    static let width = 64, height = 16

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in Self.cases {
            do {
                let buffer = try await Self.stored(gpu, c)
                let off = YcbcrTargetTest.mismatches(buffer, Self.expected(c), wide: c.tenBit)
                metrics.set("\(c.format)_luma_off", off[0])
                metrics.set("\(c.format)_chroma_off", off[1])
                metrics.set("\(c.format)_texel0", Self.texel0(buffer, c).map(String.init).joined(separator: "/"))
                checks.append(("\(c.format): every code Metal's", off == [0, 0]))
            } catch {
                checks.append(("\(c.format): \(error)", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    /// p3storeoracle.mm's floats: std::mt19937(7)'s outputs % 4096 / 4095, four a texel
    static let values: [SIMD4<Float>] = {
        let words = mt19937(seed: 7, count: 4 * width * height).map { Float($0 % 4096) / 4095 }
        return (0 ..< width * height).map {
            SIMD4(words[4 * $0], words[4 * $0 + 1], words[4 * $0 + 2], words[4 * $0 + 3])
        }
    }()

    static func mt19937(seed: UInt32, count: Int) -> [UInt32] {
        var mt = [UInt32](repeating: seed, count: 624)
        for i in 1 ..< 624 { mt[i] = 1_812_433_253 &* (mt[i - 1] ^ (mt[i - 1] >> 30)) &+ UInt32(i) }
        var out: [UInt32] = []
        while out.count < count {
            for i in 0 ..< 624 {
                let y = (mt[i] & 0x8000_0000) | (mt[(i + 1) % 624] & 0x7fff_ffff)
                mt[i] = mt[(i + 397) % 624] ^ (y >> 1) ^ (y & 1 == 0 ? 0 : 0x9908_b0df)
            }
            for var y in mt.prefix(count - out.count) {
                y ^= y >> 11
                y ^= (y << 7) & 0x9d2c_5680
                y ^= (y << 15) & 0xefc6_0000
                out.append(y ^ (y >> 18))
            }
        }
        return out
    }

    /// The planes' words as Metal stores them: luma, then Cb, Cr pairs, a 10-bit code in its word's high bits
    static func expected(_ c: Case) -> [[UInt16]] {
        let shift: UInt16 = c.tenBit ? 6 : 0
        if let constant = c.constant {
            let n = width * height
            return [
                [UInt16](repeating: constant << shift, count: n), [UInt16](repeating: constant << shift, count: 2 * n),
            ]
        }
        let codes = YcbcrStore.codes(
            values, width: width, height: height, max: c.tenBit ? 1023 : 255, halvedAcross: false, halvedDown: false,
            source: .drawnFloat
        )
        return [codes.luma.map { $0 << shift }, codes.chroma.map { $0 << shift }]
    }

    static func stored(_ gpu: GPU, _ c: Case) async throws -> CVPixelBuffer {
        guard let buffer = YcbcrCornerTest.pixelBuffer(width, height, c.fourcc),
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
              let format = MTLPixelFormat(rawValue: c.format)
        else { throw TestFailure("no surface") }
        IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, "P3_D65" as CFString)
        YcbcrTargetTest.scribble(buffer, seed: 0)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture")
        }
        let state = try gpu.render(fragment: "texel_values", format: format)
        let data = try gpu.buffer(values)
        var rowTexels = UInt32(width)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentBuffer(data, offset: 0, index: 0)
            encoder.setFragmentBytes(&rowTexels, length: 4, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        return buffer
    }

    static func texel0(_ buffer: CVPixelBuffer, _ c: Case) -> [Int] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        return [(0, 0), (1, 0), (1, 1)].map { plane, at in
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return -1 }
            let bytes = base.bindMemory(to: UInt8.self, capacity: 4)
            return c.tenBit ? (Int(bytes[2 * at]) | Int(bytes[2 * at + 1]) << 8) >> 6 : Int(bytes[at])
        }
    }
}
