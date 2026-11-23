import Foundation
import HWTestCore
import Metal

/// QuartzCore's multiply over BGRA8 (PBGRAXm_TimgBmpyA2Xhf_Isrc, its colour 1 write-masked), which the facade draws
/// unforced on rung 3 (chip_facts.h kIteratedProven "mpy"): the iterated blend on, the per-target blend off and the
/// targets narrowed to colour 0 (fetch_free.h). Each case draws it over colour 0's first quarter in one
/// pass and over its second in a later pass, with other work between (`Between`). The state has two colours, so a pass
/// of its own never keeps the blend on (RenderEncoder::keepsExtras): it sets it back and binds the state again. A
/// one-colour BGRA8 pass between keeps it on where the state's segment is kept across passes, until its clear or
/// another state's bind sets it back. The image is one opaque magenta texel, the vertex colour the same magenta (the
/// source is magenta whether the texture function samples, tints or multiplies) and colour 0 is cleared opaque cyan, so
/// the multiply (pack/iterated/reference.py apple "mpy") is blue: every channel 0 or 1, exact under any rounding
/// (rung 3's programs are within one code of exact, chip_facts.h). A draw without the blend writes magenta, a draw
/// dropped leaves cyan. The other state draws opaque yellow over the whole of a target it does not load, yellow only
/// with the blend set back.
public final class IteratedBlendPasses {
    public static let label = "PBGRAXm_TimgBmpyA2Xhf_Isrc"

    public enum Between: String, CaseIterable, Sendable {
        /// one pass, one draw: the control, right on every build
        case single
        /// the next pass in the same command buffer
        case pass
        /// the next pass in the next command buffer
        case buffer
        /// a one-colour BGRA8 pass that only clears another texture
        case clearPass = "clear_pass"
        /// the next pass clears both colours before its draw
        case clears
        /// a blit encoder copying colour 0 out
        case blit
        /// a compute encoder (its prologue sets nothing back)
        case compute
        /// a one-colour BGRA8 pass, loading nothing, drawing another state into another texture
        case state
    }

    public struct Drawn: Sendable {
        public let paths: [String]
        /// colour 0's pixels off the CPU's, and the first of them as "x,y:BGRA"
        public let wrong: Int
        public let first: String
        /// the work between's own result off (texels or words); nil where it leaves none
        public let between: Int?
        /// the draws the facade counted on rung 3 (nil without its counters), of `draws`
        public let rung3: Int?
        public let draws: Int
    }

    static let width = 64, height = 16, quarter = 16
    static let clear = SIMD4<Float>(0, 1, 1, 1)
    static let other = SIMD4<Float>(1, 1, 0, 1)
    static let image = SIMD4<Float>(1, 0, 1, 1)
    static let words = 64

    let gpu: GPU
    let state: MTLRenderPipelineState
    let formats: [MTLPixelFormat]
    let source: MTLTexture
    let sampler: MTLSamplerState
    let solid: MTLRenderPipelineState
    let fold: MTLComputePipelineState
    /// the facade counts rung-3 draws only where it has its probe channel (debug builds)
    let counts: Bool

    public init(line: PipelineLine, library: MTLLibrary, gpu: GPU) throws {
        self.gpu = gpu
        counts = (gpu.device as? NSObject)?.responds(to: NSSelectorFromString("switchMetalProbeFetchRung:")) ?? false
        state = try CAState.pipeline(line, library: library, device: gpu.device)
        formats = try line.colors.sorted { $0.index < $1.index }.map {
            guard let format = MTLPixelFormat(rawValue: $0.format) else {
                throw TestFailure("attachment format \($0.format)")
            }
            return format
        }
        source = try gpu.texture(.bgra8Unorm, 8, 8, usage: [.shaderRead], storage: .shared)
        let texels = Array([[UInt8]](repeating: Self.bgra(Self.image), count: 64).joined())
        source.replace(region: MTLRegionMake2D(0, 0, 8, 8), mipmapLevel: 0, withBytes: texels, bytesPerRow: 32)
        sampler = try CAStateSweep.linearSampler(gpu.device)
        solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        fold = try gpu.compute("icb_fold")
    }

    public func run(_ between: Between) async throws -> Drawn {
        let targets = try formats.map { try gpu.texture($0, Self.width, Self.height) }
        let aside = try gpu.texture(.bgra8Unorm, Self.width, Self.height)
        let words = try gpu.buffer((0 ..< Self.words).map { UInt32($0) })
        let counted = { self.counts ? GM20BFacade.counters(of: self.gpu.device)["fetch_rung3"] : nil }
        let before = counted()
        var paths: [String] = []
        let alone = { (clear: Bool, quarter: Int) async throws -> String in
            try await self.gpu.run(tolerateError: true) { self.pass($0, targets, clear: clear, quarter: quarter) }.path
        }
        switch between {
        case .single:
            paths.append(try await alone(true, 0))
        case .buffer:
            paths.append(try await alone(true, 0))
            paths.append(try await alone(false, 1))
        default:
            paths.append(
                try await gpu.run(tolerateError: true) { commands in
                    self.pass(commands, targets, clear: true, quarter: 0)
                    self.work(between, commands, targets[0], aside, words)
                    self.pass(commands, targets, clear: between == .clears, quarter: 1)
                }.path
            )
        }
        let draws = between == .single ? 1 : 2
        let rung3 = before.flatMap { b in counted().map { $0 - b } }
        let drawn: [Int] = between == .single ? [0] : between == .clears ? [1] : [0, 1]
        let (wrong, first) = Self.compare(
            try await gpu.read(targets[0], bytesPerPixel: 4), Self.expected(drawnQuarters: drawn)
        )
        var asideWrong: Int?
        switch between {
        case .clearPass, .state:
            asideWrong = Self.compare(try await gpu.read(aside, bytesPerPixel: 4), Self.uniform(Self.other)).wrong
        case .blit:
            let copied = try await gpu.read(aside, bytesPerPixel: 4)
            asideWrong = Self.compare(copied, Self.expected(drawnQuarters: [0])).wrong
        case .compute:
            let got = UnsafeBufferPointer(
                start: words.contents().bindMemory(to: UInt32.self, capacity: Self.words), count: Self.words
            )
            asideWrong = got.enumerated().filter { $0.element != UInt32($0.offset) * 3 + 5 }.count
        default:
            break
        }
        return Drawn(paths: paths, wrong: wrong, first: first, between: asideWrong, rung3: rung3, draws: draws)
    }

    /// One pass of the state over both its colours, a rectangle over colour 0's quarter `quarter`, its whole height
    func pass(_ commands: MTLCommandBuffer, _ targets: [MTLTexture], clear: Bool, quarter: Int) {
        let pass = MTLRenderPassDescriptor()
        for (index, target) in targets.enumerated() {
            pass.colorAttachments[index].texture = target
            pass.colorAttachments[index].loadAction = clear ? .clear : .load
            pass.colorAttachments[index].clearColor = index == 0 ? Self.clearColor(Self.clear) : MTLClearColor()
            pass.colorAttachments[index].storeAction = .store
        }
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        CAStateSweep.bind(encoder, source: source, sampler: sampler)
        let x = { (column: Int) in Float(column) / Float(Self.width) * 2 - 1 }
        var vertices = CAState.rectangle(
            x0: x(quarter * Self.quarter), y0: -1, x1: x((quarter + 1) * Self.quarter), y1: 1,
            color: SIMD4<Float16>(1, 0, 1, 1)
        )
        encoder.setVertexBytes(&vertices, length: MemoryLayout<CAState.Vertex>.stride * vertices.count, index: 1)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: vertices.count)
        encoder.endEncoding()
    }

    func work(
        _ between: Between, _ commands: MTLCommandBuffer, _ colour: MTLTexture, _ aside: MTLTexture, _ words: MTLBuffer
    ) {
        switch between {
        case .clearPass, .state:
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = aside
            // a clear would set the blend back in the prologue, ahead of the other state's bind under test
            pass.colorAttachments[0].loadAction = between == .state ? .dontCare : .clear
            pass.colorAttachments[0].clearColor = Self.clearColor(Self.other)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if between == .state {
                encoder.setRenderPipelineState(solid)
                GPU.draw(encoder, corners: GPU.fullScreen, color: Self.other)
            }
            encoder.endEncoding()
        case .blit:
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: colour, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                sourceSize: MTLSize(width: Self.width, height: Self.height, depth: 1), to: aside, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
        case .compute:
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(fold)
            encoder.setBuffer(words, offset: 0, index: 0)
            var value: UInt32 = 5
            encoder.setBytes(&value, length: 4, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: Self.words, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: min(fold.maxTotalThreadsPerThreadgroup, 64), height: 1, depth: 1)
            )
            encoder.endEncoding()
        default:
            break
        }
    }

    /// QuartzCore 24B119's multiply of premultiplied S over D (pack/iterated/reference.py apple "mpy")
    static func multiply(_ s: SIMD4<Float>, over d: SIMD4<Float>) -> SIMD4<Float> {
        var out = s * (1 - d.w) + ((1 - s.w) + s) * d
        out.w = min(max(s.w * (1 - d.w) + d.w, 0), 1)
        return out
    }

    static func bgra(_ c: SIMD4<Float>) -> [UInt8] {
        [c.z, c.y, c.x, c.w].map { UInt8((min(max($0, 0), 1) * 255).rounded()) }
    }

    static func clearColor(_ c: SIMD4<Float>) -> MTLClearColor {
        MTLClearColor(red: Double(c.x), green: Double(c.y), blue: Double(c.z), alpha: Double(c.w))
    }

    static func uniform(_ c: SIMD4<Float>) -> [UInt8] {
        Array([[UInt8]](repeating: bgra(c), count: width * height).joined())
    }

    /// Colour 0 cleared, then multiplied over in the quarters `drawnQuarters`
    static func expected(drawnQuarters: [Int]) -> [UInt8] {
        let drawn = bgra(multiply(image, over: clear)), cleared = bgra(clear)
        return (0 ..< width * height).flatMap { at in drawnQuarters.contains(at % width / quarter) ? drawn : cleared }
    }

    static func compare(_ got: [UInt8], _ want: [UInt8]) -> (wrong: Int, first: String) {
        guard got.count == want.count else { return (width * height, "size \(got.count)") }
        var wrong = 0
        var first = "-"
        for at in stride(from: 0, to: got.count, by: 4) where got[at ..< at + 4] != want[at ..< at + 4] {
            if wrong == 0 {
                let pixel = at / 4, bytes = got[at ..< at + 4].map { String($0) }
                first = "\(pixel % width),\(pixel / width):" + bytes.joined(separator: "/")
            }
            wrong += 1
        }
        return (wrong, first)
    }
}
