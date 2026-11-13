import CoreVideo
import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Render passes into the packed two-plane YCbCr attachments (YCBCR10_*_2P_PACKED, their sRGB twins, RGB10_*_2P_PACKED
/// over 'p420', 'p422', 'p444' and 'pf20'), each beside the same passes into its unpacked twin. An M4 Max's Metal faults
/// on every pass into a packed target, so the store is not measured: the raw path stores the twin's codes three to a
/// word, at bits 0, 10 and 20 as Metal reads them, bits 30-31 and a row's tail slots 0. Every word is checked against
/// that (wrong_words_luma_/wrong_words_chroma_), and the packed texture's reads against the twin's (wrong_reads_). words_
/// records the device's first words of each plane, so a run on any GPU, the raw facade's or not, keeps its codes.
struct YcbcrPackedTargetTest: HWTest {
    let id = "gpu.ycbcr_packed_target"
    let name = "Render passes into packed two-plane YCbCr surfaces"
    let subsystem = Subsystem.metal

    struct Packed {
        let format: UInt
        let fourcc: OSType
        let twin: YcbcrTargetTest.Target

        init(_ format: UInt, _ fourcc: OSType, twin: UInt, over twinFourcc: OSType) {
            self.format = format
            self.fourcc = fourcc
            self.twin = YcbcrTargetTest.Target(format: twin, fourcc: twinFourcc)
        }

        var label: String {
            let code = String(bytes: withUnsafeBytes(of: fourcc.bigEndian) { Array($0) }, encoding: .ascii) ?? "?"
            return "\(format) over '\(code)'"
        }
    }

    static let p420: OSType = 0x7034_3230, p422: OSType = 0x7034_3232, p444: OSType = 0x7034_3434
    static let pf20: OSType = 0x7066_3230
    static let x420 = kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
        x422 = kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange
    static let x444 = kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange,
        xf20 = kCVPixelFormatType_420YpCbCr10BiPlanarFullRange

    static let targets: [Packed] = [
        Packed(508, p420, twin: 505, over: x420), Packed(508, pf20, twin: 505, over: xf20),
        Packed(509, p422, twin: 506, over: x422), Packed(510, p444, twin: 507, over: x444),
        Packed(528, p420, twin: 525, over: x420), Packed(529, p422, twin: 526, over: x422),
        Packed(530, p444, twin: 527, over: x444), Packed(546, p420, twin: 543, over: x420),
        Packed(547, p422, twin: 544, over: x422), Packed(548, p444, twin: 545, over: x444),
    ]

    struct Found {
        var luma = 0, chroma = 0, reads = 0
        var words = ""
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let probe = try GPU.make()
        let onRaw = RawFacade.kind(of: probe.device) == "raw"
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for target in Self.targets {
            let gpu = try GPU.make()
            let found: Found
            let note: String?
            do {
                (found, note) = try await exercise(gpu, target)
            } catch {
                (found, note) = (Found(), "failed: \(GPU.describe(error))")
            }
            let raw = !gpu.paths.isEmpty && gpu.paths.paths.keys.allSatisfy { $0 == "raw" }
            let key = target.label.filter { $0.isLetter || $0.isNumber }
            metrics.set("paths_" + key, gpu.paths.isEmpty ? "-" : gpu.paths.text)
            metrics.set("wrong_words_luma_" + key, found.luma)
            metrics.set("wrong_words_chroma_" + key, found.chroma)
            metrics.set("wrong_reads_" + key, found.reads)
            metrics.set("words_" + key, found.words)
            var check = "\(target.label): its twin's codes three to a word, read as its twin, every command buffer raw"
            if let note { check += " (\(note))" }
            checks.append((check, note == nil && found.luma + found.chroma + found.reads == 0 && raw))
        }
        guard onRaw else {
            return .skip("the default device is not the raw facade: its words recorded", metrics)
        }
        return .judged(checks, metrics, proof: probe.proof)
    }

    /// The words off the twin's codes in each plane and the reads off the twin's, and why the passes could not run.
    func exercise(_ gpu: GPU, _ target: Packed) async throws -> (Found, String?) {
        let (w, h) = (YcbcrTargetTest.width, YcbcrTargetTest.height)
        guard let (packed, packedTexture) = Self.surface(gpu, target.fourcc, target.format, w, h),
            let (twin, twinTexture) = Self.surface(gpu, target.twin.fourcc, target.twin.format, w, h)
        else { return (Found(), "no surface or texture") }
        defer { withExtendedLifetime(twin) {} }
        let format = MTLPixelFormat(rawValue: target.format)!,
            twinFormat = MTLPixelFormat(rawValue: target.twin.format)!
        let floats = try gpu.render(fragment: "texel_values", format: format)
        let halves = try gpu.render(fragment: "texel_halves", format: format)
        let twinFloats = try gpu.render(fragment: "texel_values", format: twinFormat)
        let reader = try gpu.render(fragment: "texel_read", format: .rgba32Float)
        let values = YcbcrTargetTest.values(w * h)
        let halved = values.map { SIMD4<Float>(Float(Float16($0.x)), Float(Float16($0.y)), Float(Float16($0.z)), 1) }
        let k = YcbcrTargetTest.clear
        let clear = SIMD4<Float>(Float(k.red), Float(k.green), Float(k.blue), 1)
        let passes: [(MTLRenderPipelineState?, [SIMD4<Float>], YcbcrStore.Source)] = [
            (nil, Array(repeating: clear, count: w * h), .clear), (floats, values, .drawnFloat),
            (halves, halved, .drawnHalf),
        ]
        var found = Found()
        for (index, (state, texels, source)) in passes.enumerated() {
            YcbcrTargetTest.scribble(packed, seed: UInt8(index))
            let data = try gpu.buffer(state == nil ? [clear] : values)
            try await Self.pass(gpu, packedTexture, state, data, w)
            let codes = YcbcrTargetTest.codes(target.twin, texels, source)
            let off = Self.mismatches(packed, codes, &found.words)
            found.luma += off[0]
            found.chroma += off[1]
        }
        try await Self.pass(gpu, twinTexture, twinFloats, try gpu.buffer(values), w)
        try await Self.pass(gpu, packedTexture, floats, try gpu.buffer(values), w)
        let reads = try await [packedTexture, twinTexture].asyncMap { try await Self.read(gpu, reader, $0) }
        found.reads = zip(reads[0], reads[1]).filter { $0 != $1 }.count
        return (found, nil)
    }

    static func surface(
        _ gpu: GPU, _ fourcc: OSType, _ format: UInt, _ w: Int, _ h: Int
    )
        -> (CVPixelBuffer, MTLTexture)?
    {
        var made: CVPixelBuffer?
        let attributes =
            [kCVPixelBufferIOSurfacePropertiesKey: [:], kCVPixelBufferMetalCompatibilityKey: true] as CFDictionary
        guard CVPixelBufferCreate(nil, w, h, fourcc, attributes, &made) == kCVReturnSuccess, let buffer = made,
            let surface = CVPixelBufferGetIOSurface(buffer)?.takeUnretainedValue(),
            let pixelFormat = MTLPixelFormat(rawValue: format)
        else { return nil }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: pixelFormat, width: w, height: h, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0) else {
            return nil
        }
        return (buffer, texture)
    }

    /// A clear (no state) or a draw of `data` into `texture`.
    static func pass(
        _ gpu: GPU, _ texture: MTLTexture, _ state: MTLRenderPipelineState?, _ data: MTLBuffer, _ w: Int
    )
        async throws
    {
        var width = UInt32(w)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = state == nil ? .clear : .dontCare
            pass.colorAttachments[0].clearColor = YcbcrTargetTest.clear
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let state {
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBuffer(data, offset: 0, index: 0)
                encoder.setFragmentBytes(&width, length: 4, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            encoder.endEncoding()
        }
    }

    /// The texture's texels as a draw reads them.
    static func read(_ gpu: GPU, _ reader: MTLRenderPipelineState, _ texture: MTLTexture) async throws -> [SIMD4<Float>]
    {
        let image = try gpu.texture(.rgba32Float, texture.width, texture.height, storage: .shared)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = image
            pass.colorAttachments[0].loadAction = .dontCare
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(reader)
            var corners = GPU.fullScreen
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            encoder.setFragmentTexture(texture, index: 0)
            encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            encoder.endEncoding()
        }
        var texels = [SIMD4<Float>](repeating: .zero, count: texture.width * texture.height)
        image.getBytes(
            &texels, bytesPerRow: texture.width * 16, from: MTLRegionMake2D(0, 0, texture.width, texture.height),
            mipmapLevel: 0)
        return texels
    }

    /// Each plane's words that differ from `planes` (codes a row, three to a word); the first words of each plane into
    /// `words`.
    static func mismatches(_ buffer: CVPixelBuffer, _ planes: [[UInt16]], _ words: inout String) -> [Int] {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        var wrong = [0, 0]
        var first: [String] = []
        for (plane, codes) in planes.enumerated() {
            guard let base = CVPixelBufferGetBaseAddressOfPlane(buffer, plane) else {
                wrong[plane] = codes.count
                continue
            }
            let row = CVPixelBufferGetBytesPerRowOfPlane(buffer, plane)
            let rows = CVPixelBufferGetHeightOfPlane(buffer, plane)
            let perRow = codes.count / rows
            for y in 0..<rows {
                let line = base.advanced(by: y * row)
                for x in 0..<((perRow + 2) / 3) {
                    var want: UInt32 = 0
                    for i in 0..<3 where 3 * x + i < perRow {
                        want |= UInt32(codes[y * perRow + 3 * x + i]) << (10 * i)
                    }
                    let stored = line.loadUnaligned(fromByteOffset: 4 * x, as: UInt32.self)
                    wrong[plane] += stored == want ? 0 : 1
                    if y == 0 && x < 3 {
                        first.append((x == 0 ? (plane == 0 ? "y:" : "c:") : "") + String(stored, radix: 16))
                    }
                }
            }
        }
        words = first.joined(separator: ",")
        return wrong
    }
}

extension Array {
    fileprivate func asyncMap<T>(_ transform: (Element) async throws -> T) async rethrows -> [T] {
        var out: [T] = []
        for element in self { out.append(try await transform(element)) }
        return out
    }
}
