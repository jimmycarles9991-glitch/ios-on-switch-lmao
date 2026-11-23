import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A layered pass into a 16x16 2D array of three slices of A1BGR5Unorm (41), RGB9E5Float (93) or BGRA10_XR_sRGB (553),
/// formats the driver renders through a stand-in: every slice uploaded a pattern of words a load keeps (RGB9E5 words
/// as a draw stores them, 553's alpha codes within [0, 1]), a pass at slice 1, renderTargetArrayLength 2, Load, an
/// instanced draw (layered_vertex) of colour k over pixels [4, 12) of layer k, Store. Slice 0 keeps its pattern, slices
/// 1 and 2 hold the pattern with colour 0 or 1 inside the quad, stored as a one-sample draw stores it, tolerance 0. The
/// colours' words are an M4 Max's (this test's own pass there, which also keeps every pattern word). Per format
/// f<format>_off texels off in slices 0/1/2, f<format>_max the largest difference in codes (93: mantissa steps); every
/// command buffer raw, failed_cbs=0.
struct StandInLayeredTest: HWTest {
    let id = "gpu.gm20b.standin_layered"
    let name = "A layered pass into arrays of stand-in formats loads, draws and stores each layer, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Case {
        let format: WordMipFormat
        let colours: [SIMD4<Float>]
        let drawn: [UInt64]
    }

    static let side = 16, quad = 4 ..< 12
    static let cases = [
        Case(
            format: StandInResolveCases.formats[1],
            colours: [SIMD4(3 / 31, 17 / 31, 29 / 31, 1), SIMD4(30 / 31, 0, 12 / 31, 0)],
            drawn: [0x1c7b, 0xf018]
        ),
        Case(
            format: StandInResolveCases.formats[4], colours: [SIMD4(0.5, 0.25, 0.125, 1), SIMD4(3, 0, 1.5, 1)],
            drawn: [0x7901_0100, 0x8b00_0180]
        ),
        Case(
            format: StandInResolveCases.formats[5], colours: [SIMD4(1, 0, 0.5, 1), SIMD4(0, 1, -0.25, 0.5)],
            drawn: [0xdf80_df80_6000_bdc0, 0x9fc0_6000_df80_1b80]
        ),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in Self.cases {
            let key = c.format.key
            do {
                let slices = try await Self.layered(gpu, c)
                let judged = (0 ..< 3).map { Self.judged(c, slice: $0, slices[$0]) }
                metrics.set(key + "_off", judged.map { String($0.off) }.joined(separator: "/"))
                metrics.set(key + "_max", judged.map(\.worst).max() ?? 0)
                checks.append(
                    ("\(key): slice 0 kept, slices 1 and 2 loaded and drawn", judged.allSatisfy { $0.off == 0 })
                )
            } catch {
                checks.append(("\(key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func pattern(_ f: WordMipFormat, slice: Int) -> [UInt64] {
        var state = UInt64(f.format.rawValue) &* 7 &+ UInt64(slice)
        return (0 ..< side * side).map { _ in
            let w = WordMipmapCases.splitmix(&state)
            switch f.format.rawValue {
            case 93: return w & 0x7ff_ffff | 0x100 | (13 + (w >> 40) % 5) << 27
            case 553:
                let codes = [w & 1023, w >> 10 & 1023, w >> 20 & 1023, 384 + (w >> 30) % 511]
                return codes.enumerated().reduce(0) { $0 | $1.element << UInt64(16 * $1.offset + 6) }
            default: return w & 0xffff
            }
        }
    }

    static func bytes(_ f: WordMipFormat, _ words: [UInt64]) -> [UInt8] {
        words.flatMap { word in (0 ..< f.bytes).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
    }

    static func layered(_ gpu: GPU, _ c: Case) async throws -> [[UInt64]] {
        let f = c.format
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = f.format
        (d.width, d.height, d.arrayLength) = (side, side, 3)
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let array = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(f.key) array") }
        for slice in 0 ..< 3 {
            try await gpu.upload(
                bytes(f, pattern(f, slice: slice)), to: array, bytesPerRow: side * f.bytes, slice: slice
            )
        }
        let state = try gpu.render(vertex: "layered_vertex", fragment: "flat_color_fragment", format: f.format) {
            $0.inputPrimitiveTopology = .triangle
        }
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = array
            pass.colorAttachments[0].slice = 1
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .store
            pass.renderTargetArrayLength = 2
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(state)
            var corners = GPU.pixelQuad(
                quad.lowerBound, quad.lowerBound, quad.upperBound, quad.upperBound, width: side, height: side
            )
            var colours = c.colours
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setVertexBytes(&colours, length: MemoryLayout<SIMD4<Float>>.stride * 2, index: 1)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 2)
            encoder.endEncoding()
        }
        var slices: [[UInt64]] = []
        for slice in 0 ..< 3 {
            let got = try await gpu.read(array, bytesPerPixel: f.bytes, slice: slice)
            slices.append(stride(from: 0, to: got.count, by: f.bytes).map { f.word(got[$0 ..< $0 + f.bytes]) })
        }
        return slices
    }

    static func judged(_ c: Case, slice: Int, _ got: [UInt64]) -> (off: Int, worst: Int) {
        let kept = pattern(c.format, slice: slice)
        var judged = (off: 0, worst: 0)
        for at in got.indices {
            let inside = slice > 0 && quad.contains(at % side) && quad.contains(at / side)
            let want = inside ? c.drawn[slice - 1] : kept[at]
            judged.off += got[at] == want ? 0 : 1
            judged.worst = max(judged.worst, c.format.apart(got[at], want))
        }
        return judged
    }
}
