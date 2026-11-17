import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// An extended-range colour's half shadow (raw_xr_shadow.h): Metal's tile holds a BGR10_XR or BGRA10_XR colour as half
/// through a pass, so a blend over an earlier draw of the pass (or over a clear its codes cannot hold) reads that half, where
/// a pass without the shadow reads the stored codes. Each case runs one pass (a write then a blend, a clear then a blend, or
/// one blended state drawn twice) and hashes the target's bytes against an M4 Max's (raw/test/xrshadoworacle.mm): wrong= the
/// cases off it. split_moved= how many texels the same draws split into two passes change on this device (what a pass
/// without the shadow draws; the M4 Max: 670, 328, 679, 808, 328, 835): a 0 beside a wrong case is a missing shadow, a count
/// near the M4 Max's a blend off Metal's. Every command buffer raw.
struct XrShadowTest: HWTest {
    let id = "gpu.raw.xr_shadow"
    let name = "Blends over an earlier draw of an extended-range pass read the tile's half, as Metal's do, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    enum Kind: String {
        case write, clear, twice
    }

    struct Case {
        let format: UInt
        let bytes: Int
        let kind: Kind
        let metal: UInt64

        var key: String { "\(format == 554 ? "bgr10" : "bgra10")_\(kind.rawValue)" }
    }

    static let cases = [
        Case(format: 554, bytes: 4, kind: .write, metal: 0x2b6c_1b22_3331_155f),
        Case(format: 554, bytes: 4, kind: .clear, metal: 0x4b15_35fa_5d3c_7dc2),
        Case(format: 554, bytes: 4, kind: .twice, metal: 0xdaba_749c_3fa1_423d),
        Case(format: 552, bytes: 8, kind: .write, metal: 0x8a30_a3eb_8d8b_759b),
        Case(format: 552, bytes: 8, kind: .clear, metal: 0x37f7_7751_5a9b_5feb),
        Case(format: 552, bytes: 8, kind: .twice, metal: 0xc841_e5a7_94de_ce30),
    ]
    static let width = 64, height = 32
    static let clear = MTLClearColor(red: 0.123, green: -0.3, blue: 1.1, alpha: 0.6)

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        guard RawFacade.kind(of: probe.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: probe.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var wrong: [String] = []
        var moved: [String] = []
        var paths: [String: Int] = [:]
        for c in Self.cases {
            let gpu = try GPU.make()
            var note: String?
            var whole: [UInt8] = []
            var split: [UInt8] = []
            do {
                whole = try await exercise(gpu, c, split: false)
                split = try await exercise(gpu, c, split: true)
            } catch {
                note = "failed: \(GPU.describe(error))"
            }
            for (path, count) in gpu.paths.paths { paths[path, default: 0] += count }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
            let ok = note == nil && Self.hash(whole) == c.metal
            if !ok { wrong.append(c.key) }
            moved.append("\(Self.texelsMoved(whole, split, c.bytes))")
            var check = "\(c.key): the pass's bytes are Metal's, raw"
            if let note { check += " (\(note))" }
            checks.append((check, ok && raw))
        }
        metrics.set("route", paths.keys.sorted() == ["raw"] ? "raw" : paths.keys.sorted().joined(separator: "+"))
        metrics.set("wrong", wrong.isEmpty ? "-" : wrong.joined(separator: ","))
        metrics.set("split_moved", moved.joined(separator: ","))
        return .judged(checks, metrics, proof: probe.proof)
    }

    /// The target's bytes after the case's pass, or (split) after the same draws in two passes
    func exercise(_ gpu: GPU, _ c: Case, split: Bool) async throws -> [UInt8] {
        let (w, h) = (Self.width, Self.height)
        guard let format = MTLPixelFormat(rawValue: c.format) else { throw TestFailure("no format") }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.renderTarget]
        descriptor.storageMode = .shared
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else { throw TestFailure("no texture") }
        let zero = [UInt8](repeating: 0, count: w * h * c.bytes)
        texture.replace(
            region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: zero, bytesPerRow: w * c.bytes)
        let plain = try gpu.render(fragment: "texel_values", format: format)
        let blended = try gpu.render(fragment: "texel_values", format: format) {
            let a = $0.colorAttachments[0]!
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .sourceAlpha
            a.destinationRGBBlendFactor = .oneMinusSourceAlpha
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .oneMinusSourceAlpha
        }
        let first = try gpu.buffer(Self.values(seed: 1)), second = try gpu.buffer(Self.values(seed: 2))
        let all: [(MTLRenderPipelineState, MTLBuffer)] =
            switch c.kind {
            case .write: [(plain, first), (blended, second)]
            case .clear: [(blended, second)]
            case .twice: [(blended, first), (blended, second)]
            }
        let draws: [[(MTLRenderPipelineState, MTLBuffer)]] =
            !split ? [all] : c.kind == .clear ? [[], all] : [[all[0]], [all[1]]]
        let cleared = c.kind == .clear
        for (index, passDraws) in draws.enumerated() {
            var width = UInt32(w)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = cleared && index == 0 ? .clear : .load
                pass.colorAttachments[0].clearColor = Self.clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                for (state, values) in passDraws {
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBuffer(values, offset: 0, index: 0)
                    encoder.setFragmentBytes(&width, length: 4, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
        }
        var out = [UInt8](repeating: 0, count: w * h * c.bytes)
        texture.getBytes(&out, bytesPerRow: w * c.bytes, from: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0)
        return out
    }

    static func splitmix(_ s: inout UInt64) -> UInt64 {
        s = s &+ 0x9e37_79b9_7f4a_7c15
        var z = s
        z = (z ^ (z >> 30)) &* 0xbf58_476d_1ce4_e5b9
        z = (z ^ (z >> 27)) &* 0x94d0_49bb_1331_11eb
        return z ^ (z >> 31)
    }

    /// xrshadoworacle.mm values: colours across [-0.75, 1.25], alpha across [0, 1]
    static func values(seed: UInt64) -> [SIMD4<Float>] {
        var state = seed
        func next(_ alpha: Bool) -> Float {
            let u = Double(splitmix(&state) >> 11) / Double(UInt64(1) << 53)
            return Float(alpha ? u : u * 2.0 - 0.75)
        }
        return (0..<(width * height)).map { _ in
            let (r, g, b, a) = (next(false), next(false), next(false), next(true))
            return SIMD4(r, g, b, a)
        }
    }

    static func hash(_ bytes: [UInt8]) -> UInt64 {
        var h: UInt64 = 0xcbf2_9ce4_8422_2325
        for b in bytes { h = (h ^ UInt64(b)) &* 0x100_0000_01b3 }
        return h
    }

    static func texelsMoved(_ a: [UInt8], _ b: [UInt8], _ bytes: Int) -> Int {
        guard a.count == b.count, !a.isEmpty else { return -1 }
        return stride(from: 0, to: a.count, by: bytes).filter { a[$0..<$0 + bytes] != b[$0..<$0 + bytes] }.count
    }
}
