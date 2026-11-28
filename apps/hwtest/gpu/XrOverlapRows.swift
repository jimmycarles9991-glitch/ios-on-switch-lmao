import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// RGB10A8_2P_XR10 (550) over 'b3a8' planes whose rows overlap inside a plane (a pitch below a row's bytes), as an M4
/// Max reads and stores them (gpu.gm20b.xr_overlap_rows). Read scenes, every byte SurfaceReads.pattern: a kernel reads
/// texel (x, y) as the word at y * rows0 + 4x with the alpha byte at plane 1 + y * rows1 + x. Written scenes, every
/// byte 0xee first: a pass drawing each texel's row y as red code 385 + y, its column as green code 385 + x and alpha
/// byte y (xr_overlap_rows_frag) leaves each word and alpha byte that several rows reach with the row whose store lands
/// last (overlap_order.h overlapOrder, the order of an M4 Max's YCbCr stores); a kernel storing the same leaves one
/// of the rows that reach it; a Clear of (0.5, 0.25, -0.25, 0.75) leaves every word 0x27f80100 and alpha byte 0xbf;
/// none changes a byte outside the planes' rows.
public enum XrOverlapRows {
    typealias P = SurfaceReads.Plane

    public struct Scene: Sendable {
        public let key: String
        let width, height: Int
        let planes: [P]
        let allocation: Int
        /// The rows a texture of the scene reads: the words' and the alpha bytes' offsets and pitches
        let words: (at: Int, pitch: Int)
        let alpha: (at: Int, pitch: Int)
    }

    public static let reads = [
        Scene(
            key: "rows64_16", width: 32, height: 8,
            planes: [P(at: 0, 32, 8, bpe: 2, rows: 64), P(at: 1024, 16, 8, bpe: 1, rows: 16)], allocation: 2048,
            words: (0, 64), alpha: (1024, 16)
        ),
        Scene(
            key: "rows64_32", width: 32, height: 8,
            planes: [P(at: 0, 32, 8, bpe: 2, rows: 64), P(at: 512, 32, 8, bpe: 1, rows: 32)], allocation: 1024,
            words: (0, 64), alpha: (512, 32)
        ),
        Scene(
            key: "planes32x4", width: 32, height: 8,
            planes: [P(at: 0, 32, 4, bpe: 4, rows: 128), P(at: 512, 32, 4, bpe: 1, rows: 32)], allocation: 1024,
            words: (0, 128), alpha: (512, 32)
        ),
    ]

    public static let written = [
        reads[0],
        Scene(
            key: "rows128_64", width: 64, height: 64,
            planes: [P(at: 0, 64, 64, bpe: 2, rows: 128), P(at: 16384, 64, 64, bpe: 1, rows: 64)], allocation: 32768,
            words: (0, 128), alpha: (16384, 64)
        ),
    ]

    public struct Reading: Sendable {
        /// Word groups and alpha bytes not the expected row's (pass), not any reaching row's (kernel), not the clear's
        public var passOff = 0, kernelUncovered = 0, clearOff = 0
        /// Bytes outside the planes' rows changed by the pass, the kernel and the clear
        public var outside = 0
        public var first = "-"
        public var paths: [String] = []
    }

    static let blank: UInt8 = 0xee
    static let clearWord: UInt32 = 0x27f8_0100, clearAlpha: UInt8 = 0xbf

    /// Texels a kernel reads off the scene's rows; nil where no texture was made
    public static func readOff(_ gpu: GPU, _ s: Scene) async throws -> Int? {
        guard let surface = SurfaceReads.surface("b3a8", s.width, s.height, planes: s.planes, allocation: s.allocation)
        else { throw TestFailure("no 'b3a8' for \(s.key)") }
        guard let texture = SurfaceReads.texture(gpu, 550, s.width, s.height, over: surface) else { return nil }
        let start = SurfaceReads.bytes(surface)
        let got = try await SurfaceReads.read(gpu, texture)
        return SurfaceReads.off(got) { i in
            let (x, y) = (i % s.width, i / s.width)
            return SurfaceReads.xr(
                SurfaceReads.word(start, s.words.at + y * s.words.pitch + 4 * x),
                alpha: Float(start[s.alpha.at + y * s.alpha.pitch + x]) / 255
            )
        }
    }

    static func word(_ x: Int, _ y: Int) -> UInt32 { UInt32(385 + y) << 20 | UInt32(385 + x) << 10 | 384 }

    /// The rows reaching byte `a` of a plane whose rows of `row` bytes lie `pitch` apart, each with its byte's offset
    static func reaching(_ a: Int, pitch: Int, row: Int, rows: Int) -> [(row: Int, offset: Int)] {
        let last = min(a / pitch, rows - 1), first = a >= row ? (a - row) / pitch + 1 : 0
        return first <= last ? (first ... last).map { ($0, a - $0 * pitch) } : []
    }

    /// Each word group and alpha byte of the planes' rows, with the rows that reach it: (where, the value each row
    /// stores there and its store order, is it a word)
    static func spans(_ s: Scene) -> [(at: Int, candidates: [(value: UInt32, order: Int)], word: Bool)] {
        var out: [(at: Int, candidates: [(value: UInt32, order: Int)], word: Bool)] = []
        let wordSpan = s.words.pitch * (s.height - 1) + 4 * s.width
        for a in stride(from: 0, to: wordSpan, by: 4) {
            let c = reaching(a, pitch: s.words.pitch, row: 4 * s.width, rows: s.height)
            let stores = c.map { (word($0.offset / 4, $0.row), overlapOrder($0.offset / 4, $0.row)) }
            out.append((s.words.at + a, stores, true))
        }
        for a in 0 ..< (s.alpha.pitch * (s.height - 1) + s.width) {
            let c = reaching(a, pitch: s.alpha.pitch, row: s.width, rows: s.height)
            out.append((s.alpha.at + a, c.map { (UInt32($0.row), overlapOrder($0.offset, $0.row)) }, false))
        }
        return out
    }

    static func overlapOrder(_ x: Int, _ y: Int) -> Int { YcbcrOverlapRows.order(x, y) }

    public static func observe(_ gpu: GPU, _ s: Scene) async throws -> Reading? {
        guard let surface = SurfaceReads.surface(
            "b3a8", s.width, s.height, planes: s.planes, allocation: s.allocation, fill: { _ in blank }
        )
        else { throw TestFailure("no 'b3a8' for \(s.key)") }
        guard let texture = SurfaceReads.texture(
            gpu, 550, s.width, s.height, over: surface, usage: [.shaderRead, .shaderWrite, .renderTarget]
        )
        else { return nil }
        let state = try gpu.render(fragment: "xr_overlap_rows_frag", format: texture.pixelFormat)
        let kernel = try gpu.compute("xr_overlap_rows_kernel")
        let spans = spans(s)
        var inside = Set<Int>()
        for span in spans { for k in 0 ..< (span.word ? 4 : 1) { inside.insert(span.at + k) } }
        var reading = Reading()
        for arm in ["pass", "kernel", "clear"] {
            reset(surface)
            let timing = try await gpu.run(tolerateError: true) { commands in
                if arm == "kernel" {
                    let encoder = commands.makeComputeCommandEncoder()!
                    encoder.setComputePipelineState(kernel)
                    encoder.setTexture(texture, index: 0)
                    encoder.dispatchThreads(
                        MTLSize(width: s.width, height: s.height, depth: 1),
                        threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                    )
                    encoder.endEncoding()
                    return
                }
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = texture
                pass.colorAttachments[0].loadAction = arm == "clear" ? .clear : .dontCare
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.5, green: 0.25, blue: -0.25, alpha: 0.75)
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                if arm == "pass" {
                    var corners = GPU.fullScreen
                    encoder.setRenderPipelineState(state)
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
            reading.paths.append(timing.facadePath)
            let bytes = SurfaceReads.bytes(surface)
            for span in spans {
                let got = span.word ? SurfaceReads.word(bytes, span.at) : UInt32(bytes[span.at])
                let wrong: Bool
                switch arm {
                case "pass": wrong = got != span.candidates.max { $0.order < $1.order }?.value
                case "kernel": wrong = !span.candidates.contains { $0.value == got }
                default: wrong = got != (span.word ? clearWord : UInt32(clearAlpha))
                }
                guard wrong else { continue }
                switch arm {
                case "pass": reading.passOff += 1
                case "kernel": reading.kernelUncovered += 1
                default: reading.clearOff += 1
                }
                if reading.first == "-" { reading.first = "\(arm)@\(span.at):\(String(got, radix: 16))" }
            }
            reading.outside += bytes.indices.filter { !inside.contains($0) && bytes[$0] != blank }.count
        }
        return reading
    }

    static func reset(_ surface: IOSurfaceRef) {
        IOSurfaceLock(surface, [], nil)
        memset(IOSurfaceGetBaseAddress(surface), Int32(blank), IOSurfaceGetAllocSize(surface))
        IOSurfaceUnlock(surface, [], nil)
    }
}
