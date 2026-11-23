import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Passes that load a two-plane YCbCr target (it went to m2v: gm20b:pass.ycbcr). The driver loads each read's tile
/// value into a stand-in, draws the pass there, and stores only the 32x32 tiles a draw reaches (ycbcr_load.h). Each
/// case's planes start as random codes and take one draw into the scissor (13, 9, 30, 20) of a 64x48 surface: their
/// bytes hash to what an M4 Max's Metal stored (harness/ycbcrloadoracle.mm golden; wrong= the cases that do not), no
/// byte of the untouched tiles changes (untouched_changed), a loaded pass without a draw changes nothing
/// (undrawn_changed), and every command buffer is raw. 500_p3 is the '420f' surface under the P3_D65 matrix, its hash
/// this scene's own on an M4 Max (the oracle's golden cases hold no P3 one).
struct YcbcrLoadTest: HWTest {
    let id = "gpu.gm20b.ycbcr_load"
    let name = "Loaded passes into two-plane YCbCr targets store the tiles their draws reach, as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    struct Case {
        let format: UInt
        let fourcc: OSType
        let matrix: CFString?
        let metal: UInt64

        var key: String { matrix.map { $0 as String } == "P3_D65" ? "\(format)_p3" : "\(format)" }
        var tenBit: Bool { [505, 506, 507, 525, 526, 527, 543, 544, 545].contains(format) }
        var across: Bool { ![503, 507, 523, 527, 542, 545].contains(format) }
        var down: Bool { [500, 505, 520, 525, 540, 543].contains(format) }
    }

    static let cases = [
        Case(
            format: 540, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, matrix: nil,
            metal: 0x11ba_a8cf_6c3e_05b9
        ),
        Case(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            matrix: kCVImageBufferYCbCrMatrix_ITU_R_709_2, metal: 0x9310_1cdc_e779_635d
        ),
        Case(
            format: 520, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, matrix: nil,
            metal: 0x1b55_873b_c381_acda
        ),
        Case(
            format: 545, fourcc: kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange, matrix: nil,
            metal: 0x763b_3498_63a7_149f
        ),
        Case(
            format: 503, fourcc: kCVPixelFormatType_444YpCbCr8BiPlanarFullRange, matrix: nil,
            metal: 0xa37b_52d8_9dc1_71b7
        ),
        Case(
            format: 541, fourcc: kCVPixelFormatType_422YpCbCr8BiPlanarVideoRange, matrix: nil,
            metal: 0x96cf_2d54_459e_13a9
        ),
        Case(
            format: 543, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarFullRange, matrix: nil,
            metal: 0xf9c5_67a9_6d4f_d656
        ),
        Case(
            format: 525, fourcc: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            matrix: kCVImageBufferYCbCrMatrix_ITU_R_2020, metal: 0x329f_4c9f_31b0_4da9
        ),
        Case(
            format: 527, fourcc: kCVPixelFormatType_444YpCbCr10BiPlanarFullRange, matrix: nil,
            metal: 0x8d58_04ad_7f5b_74cd
        ),
        Case(
            format: 506, fourcc: kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange,
            matrix: kCVImageBufferYCbCrMatrix_ITU_R_709_2, metal: 0x31e8_3eb4_5b2f_622b
        ),
        Case(
            format: 500, fourcc: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            matrix: "P3_D65" as CFString, metal: 0x9a23_3bc3_a4df_4bdd
        ),
    ]
    static let width = 64, height = 48
    static let rect = MTLScissorRect(x: 13, y: 9, width: 30, height: 20)

    struct Result {
        var hashOk = false
        var untouched = 0
        var undrawn = 0
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        guard GM20BFacade.kind(of: probe.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: probe.device)))")
        }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var wrong: [String] = []
        var untouched = 0, undrawn = 0
        var paths: [String: Int] = [:]
        for c in Self.cases {
            let gpu = try GPU.make()
            var result = Result()
            var note: String?
            do {
                result = try await exercise(gpu, c)
            } catch {
                note = "failed: \(GPU.describe(error))"
            }
            for (path, count) in gpu.paths.paths { paths[path, default: 0] += count }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "gm20b" }
            if !result.hashOk { wrong.append(c.key) }
            untouched += result.untouched
            undrawn += result.undrawn
            var check = "\(c.key): Metal's bytes, untouched tiles kept, a pass without a draw stores nothing, raw"
            if let note { check += " (\(note))" }
            checks.append((check, note == nil && result.hashOk && result.untouched + result.undrawn == 0 && raw))
        }
        metrics.set("route", paths.keys.sorted() == ["gm20b"] ? "gm20b" : paths.keys.sorted().joined(separator: "+"))
        metrics.set("wrong", wrong.isEmpty ? "-" : wrong.joined(separator: ","))
        metrics.set("untouched_changed", untouched)
        metrics.set("undrawn_changed", undrawn)
        return .judged(checks, metrics, proof: probe.proof)
    }

    func exercise(_ gpu: GPU, _ c: Case) async throws -> Result {
        let (w, h) = (Self.width, Self.height)
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, c.fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made,
              let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
              let format = MTLPixelFormat(rawValue: c.format)
        else { throw TestFailure("no surface") }
        if let matrix = c.matrix { IOSurfaceSetValue(surface, "IOSurfaceYCbCrMatrix" as CFString, matrix) }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            throw TestFailure("no texture")
        }
        let state = try gpu.render(fragment: "texel_values", format: format)
        let data = try gpu.buffer(Self.values(w * h, seed: UInt64(c.format) * 7 + UInt64(h)))
        let rows = Self.rows(c)
        var result = Result()
        for drawn in [false, true] {
            let before = Self.fill(buffer, rows, seed: UInt64(c.format) * 131 + UInt64(w))
            var width = UInt32(w)
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = .load
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                if drawn {
                    encoder.setRenderPipelineState(state)
                    encoder.setScissorRect(Self.rect)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.setFragmentBuffer(data, offset: 0, index: 0)
                    encoder.setFragmentBytes(&width, length: 4, index: 1)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            let after = Self.read(buffer, rows)
            if !drawn {
                result.undrawn = zip(before.joined(), after.joined()).filter { $0 != $1 }.count
                continue
            }
            result.hashOk = Self.hash(after) == c.metal
            result.untouched = Self.untouchedChanged(c, rows, before, after)
        }
        return result
    }

    /// Each plane's row bytes and rows: a luma code a texel, a Cb, Cr pair a chroma texel, two bytes a 10-bit code.
    static func rows(_ c: Case) -> [(bytes: Int, count: Int)] {
        let code = c.tenBit ? 2 : 1
        let pairs = c.across ? width / 2 : width
        return [(width * code, height), (2 * pairs * code, c.down ? (height + 1) / 2 : height)]
    }

    static func splitmix(_ s: inout UInt64) -> UInt64 {
        s = s &+ 0x9e37_79b9_7f4a_7c15
        var z = s
        z = (z ^ (z >> 30)) &* 0xbf58_476d_1ce4_e5b9
        z = (z ^ (z >> 27)) &* 0x94d0_49bb_1331_11eb
        return z ^ (z >> 31)
    }

    /// ycbcr_load_cases.h randomPlanes: the planes' row bytes from one generator, plane 0 first.
    static func fill(_ buffer: CVPixelBuffer, _ rows: [(bytes: Int, count: Int)], seed: UInt64) -> [[UInt8]] {
        var state = seed
        let planes = rows
            .map { r in (0 ..< r.bytes * r.count).map { _ in UInt8(truncatingIfNeeded: splitmix(&state)) } }
        CVPixelBufferLockBaseAddress(buffer, [])
        defer { CVPixelBufferUnlockBaseAddress(buffer, []) }
        for (plane, r) in rows.enumerated() {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { continue }
            let pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            for y in 0 ..< r.count {
                planes[plane].withUnsafeBytes { src in
                    (base + y * pitch).copyMemory(from: src.baseAddress! + y * r.bytes, byteCount: r.bytes)
                }
            }
        }
        return planes
    }

    static func read(_ buffer: CVPixelBuffer, _ rows: [(bytes: Int, count: Int)]) -> [[UInt8]] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        return rows.enumerated().map { plane, r in
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return [] }
            let pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            return (0 ..< r.count).flatMap { y in
                Array(UnsafeRawBufferPointer(start: base + y * pitch, count: r.bytes))
            }
        }
    }

    static func hash(_ planes: [[UInt8]]) -> UInt64 {
        var h: UInt64 = 0xcbf2_9ce4_8422_2325
        for b in planes.joined() { h = (h ^ UInt64(b)) &* 0x100_0000_01b3 }
        return h
    }

    /// Bytes changed in the tiles the rectangle does not touch (a chroma block in its first luma texel's tile).
    static func untouchedChanged(_ c: Case, _ rows: [(bytes: Int, count: Int)], _ a: [[UInt8]], _ b: [[UInt8]]) -> Int {
        func reached(_ x: Int, _ y: Int) -> Bool {
            let (tx, ty) = (x / 32 * 32, y / 32 * 32)
            return tx < rect.x + rect.width && rect.x < tx + 32 && ty < rect.y + rect.height && rect.y < ty + 32
        }
        let code = c.tenBit ? 2 : 1
        var changed = 0
        for plane in 0 ..< 2 {
            for i in 0 ..< a[plane].count where a[plane][i] != b[plane][i] {
                let (y, x) = (i / rows[plane].bytes, i % rows[plane].bytes / code)
                let pixel = plane == 0 ? (x, y) : (x / 2 * (c.across ? 2 : 1), y * (c.down ? 2 : 1))
                if !reached(pixel.0, pixel.1) { changed += 1 }
            }
        }
        return changed
    }

    /// ycbcr_load_cases.h drawnValues: floats beside the grids' midpoints and half midpoints, and across [-0.05, 1.05].
    static func values(_ count: Int, seed: UInt64) -> [SIMD4<Float>] {
        var state = seed
        let grids: [UInt64] = [255, 1023, 2048, 8192]
        func value() -> Float {
            let k = splitmix(&state)
            var f: Float
            switch k % 3 {
            case 0:
                let g = grids[Int((k >> 2) % 4)]
                f = Float((Double((k >> 8) % g) + 0.5) / Double(g))
            case 1:
                let h = UInt16((k >> 8) % 0x3c00)
                f = Float((Double(Float16(bitPattern: h)) + Double(Float16(bitPattern: h + 1))) / 2)
            default:
                f = Float(Double((k >> 11) % 1_100_001) / 1_000_000.0 - 0.05)
            }
            return Float(bitPattern: f.bitPattern &+ UInt32(bitPattern: Int32((k >> 40) % 5) - 2))
        }
        return (0 ..< count).map { _ in
            let (r, g, b) = (value(), value(), value())
            return SIMD4(r, g, b, 1)
        }
    }
}
