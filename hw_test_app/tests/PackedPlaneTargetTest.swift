import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Render passes into R10Unorm_PACKED and RG10Unorm_PACKED over the planes of packed 10-bit surfaces ('p420', 'pf20',
/// 'p422', 'p444'), as VideoStabilizationV2 renders its frames on iOS 17. An M4 Max's Metal faults on these passes, so the
/// store is not measured: the raw path stores each channel's code as the 10-bit YCbCr targets hold it (YcbcrStore.code),
/// three to a word in the layout Metal reads, bits 30-31 and a row's tail slots 0. Every word is checked against that
/// (wrong_words_), each texture's reads against an RGB10A2Unorm texture of the same codes (wrong_reads_), and every
/// command buffer must be raw (device log: "raw: packed 10-bit plane stored on the GPU"). words_ keeps the device's first
/// words of each view, so a run on any GPU keeps its codes.
struct PackedPlaneTargetTest: HWTest {
    let id = "gpu.packed_plane_target"
    let name = "Render passes into packed 10-bit surface planes"
    let subsystem = Subsystem.metal

    struct View {
        let format: UInt
        let fourcc: OSType

        var plane: Int { Int(format) - 588 }
        var channels: Int { plane + 1 }
        var label: String {
            let code = String(bytes: withUnsafeBytes(of: fourcc.bigEndian) { Array($0) }, encoding: .ascii) ?? "?"
            return "\(format) over '\(code)' plane \(plane)"
        }
    }

    static let views: [View] = [
        YcbcrPackedTargetTest.p420, YcbcrPackedTargetTest.pf20, YcbcrPackedTargetTest.p422,
        YcbcrPackedTargetTest.p444,
    ].flatMap { fourcc in [588, 589].map { View(format: $0, fourcc: fourcc) } }

    struct Found {
        var words = 0, reads = 0
        var first = ""
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        let onRaw = RawFacade.kind(of: probe.device) == "raw"
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for view in Self.views {
            let gpu = try GPU.make()
            let found: Found
            let note: String?
            do {
                (found, note) = try await exercise(gpu, view)
            } catch {
                (found, note) = (Found(), "failed: \(GPU.describe(error))")
            }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
            let key = view.label.filter { $0.isLetter || $0.isNumber }
            metrics.set("paths_" + key, gpu.paths.isEmpty ? "-" : gpu.paths.text)
            metrics.set("wrong_words_" + key, found.words)
            metrics.set("wrong_reads_" + key, found.reads)
            metrics.set("words_" + key, found.first)
            var check = "\(view.label): its codes three to a word as Metal reads them, every command buffer raw"
            if let note { check += " (\(note))" }
            checks.append((check, note == nil && found.words + found.reads == 0 && raw))
        }
        guard onRaw else {
            return .skip("the default device is not the raw facade: its words recorded", metrics)
        }
        return .judged(checks, metrics, proof: probe.proof)
    }

    func exercise(_ gpu: GPU, _ view: View) async throws -> (Found, String?) {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard
            CVPixelBufferCreate(
                nil, YcbcrTargetTest.width, YcbcrTargetTest.height, view.fourcc, attributes, &made) == kCVReturnSuccess,
            let buffer = made, let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let format = MTLPixelFormat(rawValue: view.format)
        else { return (Found(), "no surface") }
        let (w, h) = (
            CVPixelBufferGetWidthOfPlane(buffer, view.plane), CVPixelBufferGetHeightOfPlane(buffer, view.plane)
        )
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: w, height: h, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: view.plane) else {
            return (Found(), "no texture")
        }
        let floats = try gpu.render(fragment: "texel_values", format: format)
        let halves = try gpu.render(fragment: "texel_halves", format: format)
        let reader = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        let values = YcbcrTargetTest.values(w * h)
        let halved = values.map { SIMD4<Float>(Float(Float16($0.x)), Float(Float16($0.y)), Float(Float16($0.z)), 1) }
        let k = YcbcrTargetTest.clear
        let clear = SIMD4<Float>(Float(k.red), Float(k.green), Float(k.blue), 1)
        let passes: [(MTLRenderPipelineState?, [SIMD4<Float>], YcbcrStore.Source)] = [
            (nil, Array(repeating: clear, count: w * h), .clear), (halves, halved, .drawnHalf),
            (floats, values, .drawnFloat),
        ]
        var found = Found()
        var codes: [UInt16] = []
        for (index, (state, texels, source)) in passes.enumerated() {
            YcbcrTargetTest.scribble(buffer, seed: UInt8(index))
            try await YcbcrPackedTargetTest.pass(
                gpu, texture, state, try gpu.buffer(state == nil ? [clear] : values), w)
            codes = texels.flatMap { texel in
                (0..<view.channels).map { UInt16(YcbcrStore.code(texel[$0], source, max: 1023)) }
            }
            found.words += Self.mismatches(buffer, view.plane, codes, rows: h, &found.first)
        }
        let twin = try gpu.texture(.rgb10a2Unorm, w, h, usage: [.shaderRead], storage: .shared)
        let packed: [UInt32] = (0..<(w * h)).map { i in
            let r = UInt32(codes[view.channels * i]), g = view.channels == 2 ? UInt32(codes[2 * i + 1]) : 0
            return r | g << 10 | 3 << 30
        }
        twin.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: packed, bytesPerRow: w * 4)
        let reads = try await YcbcrPackedTargetTest.read(gpu, reader, texture),
            twinReads = try await YcbcrPackedTargetTest.read(gpu, reader, twin)
        found.reads = zip(reads, twinReads).filter { $0 != $1 }.count
        return (found, nil)
    }

    /// The plane's words that differ from `codes` (a row's codes three to a word, every other bit 0); its first words
    /// into `first`.
    static func mismatches(
        _ buffer: CVPixelBuffer, _ plane: Int, _ codes: [UInt16], rows: Int, _ first: inout String
    )
        -> Int
    {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else { return codes.count }
        let pitch = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane), perRow = codes.count / rows
        var wrong = 0
        var words: [String] = []
        for y in 0..<rows {
            for x in 0..<((perRow + 2) / 3) {
                var want: UInt32 = 0
                for i in 0..<3 where 3 * x + i < perRow {
                    want |= UInt32(codes[y * perRow + 3 * x + i]) << (10 * i)
                }
                let stored = base.loadUnaligned(fromByteOffset: y * pitch + 4 * x, as: UInt32.self)
                wrong += stored == want ? 0 : 1
                if y == 0 && x < 3 { words.append(String(stored, radix: 16)) }
            }
        }
        first = words.joined(separator: ",")
        return wrong
    }
}
