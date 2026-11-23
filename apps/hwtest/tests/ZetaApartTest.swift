import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Depth and stencil attached apart: each aspect lands in its own texture, level or slice and nothing else changes.
/// 32x32 unless named; a draw writes depth 0.5 (Always, write) and stencil 7 (Replace), a pass with no draw clears to
/// 0.75 and 9. a: Depth32Float_Stencil8 A (.1/1 before) as depth, B (.2/2) as stencil, drawn: A .5/1, B .2/7;
/// a_cleared: A .75/1, B .2/9. b: a 64x64 two-level Depth32Float_Stencil8, depth level 0 (.1/1), stencil level 1
/// (.2/2), drawn: level 0 depth .5 in [0, 32)^2 and .1 elsewhere, stencil 1, level 1 .2/7; b_cleared: level 0 .75/1,
/// level 1 .2/9. c and c_cleared: b's with two slices of a 32x32 array. d: Depth32Float_Stencil8 A (.1/1) + Stencil8 B
/// (2), cleared: A .75/1, B 9; Depth32Float A (.1) + Depth32Float_Stencil8 B (.2/2), cleared: A .75, B .2/9. e:
/// Depth32Float 64x64 (.1) + Stencil8 32x32 (2) + RGBA8Unorm 64x64 (0x22 bytes before), drawn white: depth .5 in [0,
/// 32)^2 and .1 elsewhere, stencil 7, colour white in [0, 32)^2 and 0x22 elsewhere. f: Depth32Float_Stencil8 A (.1/1)
/// as depth and its X32_Stencil8 view as stencil, drawn: .5/7; f_alone: the view alone as stencil: .1/7. Tolerance 0.
/// The expectations are an M4 Max's (m2v-passes3 probe/p3probe.mm, out1.txt and out2.txt; this scene on an M4 Max: 0
/// off in every case). Per case <case>_depth_off and <case>_stencil_off texels off (e: e_colour_off too); every command
/// buffer raw, failed_cbs=0.
struct ZetaApartTest: HWTest {
    let id = "gpu.gm20b.zeta_apart"
    let name = "Depth and stencil attached apart each keep to their own texture, level or slice, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    enum Aspect { case depth, stencil, colour }

    struct Part {
        let texture: MTLTexture
        var level = 0
        var slice = 0
    }

    struct Check {
        let aspect: Aspect
        let part: Part
        let want: (Int, Int) -> UInt32
    }

    static let side = 32
    static let cases: [(key: String, run: (GPU) async throws -> [Check])] = [
        ("a", { try await twoTextures($0, drawn: true) }), ("a_cleared", { try await twoTextures($0, drawn: false) }),
        ("b", { try await twoParts($0, slices: false, drawn: true) }),
        ("b_cleared", { try await twoParts($0, slices: false, drawn: false) }),
        ("c", { try await twoParts($0, slices: true, drawn: true) }),
        ("c_cleared", { try await twoParts($0, slices: true, drawn: false) }), ("d", mixed), ("e", sizes),
        ("f", { try await stencilView($0, alone: false) }), ("f_alone", { try await stencilView($0, alone: true) }),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for c in Self.cases {
            do {
                var off: [Aspect: Int] = [:]
                for check in try await c.run(gpu) { off[check.aspect, default: 0] += try await Self.off(gpu, check) }
                metrics.set(c.key + "_depth_off", off[.depth] ?? 0)
                metrics.set(c.key + "_stencil_off", off[.stencil] ?? 0)
                if let colour = off[.colour] { metrics.set(c.key + "_colour_off", colour) }
                checks.append(
                    ("\(c.key): each aspect in its own part, nothing else touched", off.values.allSatisfy { $0 == 0 })
                )
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func depth(_ value: Double) -> UInt32 { Float(value).bitPattern }

    static func inner(_ x: Int, _ y: Int) -> Bool { x < side && y < side }

    static func zeta(
        _ gpu: GPU, _ format: MTLPixelFormat, side: Int = side, levels: Int = 1, slices: Int = 1,
        usage: MTLTextureUsage = [.renderTarget, .shaderRead]
    ) throws -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = slices > 1 ? .type2DArray : .type2D
        d.pixelFormat = format
        (d.width, d.height, d.mipmapLevelCount, d.arrayLength) = (side, side, levels, slices)
        d.usage = usage
        d.storageMode = .private
        guard let t = gpu.device.makeTexture(descriptor: d) else {
            throw TestFailure("no texture of \(format.rawValue)")
        }
        return t
    }

    static func hasDepth(_ format: MTLPixelFormat) -> Bool {
        format == .depth32Float || format == .depth32Float_stencil8
    }

    static func hasStencil(_ format: MTLPixelFormat) -> Bool {
        format == .stencil8 || format == .depth32Float_stencil8 || format == .x32_stencil8
    }

    static func attach(_ a: MTLRenderPassAttachmentDescriptor, _ part: Part, load: MTLLoadAction) {
        a.texture = part.texture
        a.level = part.level
        a.slice = part.slice
        a.loadAction = load
        a.storeAction = .store
    }

    static func clear(_ gpu: GPU, _ part: Part, depth: Double, stencil: UInt32) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            if hasDepth(part.texture.pixelFormat) {
                attach(pass.depthAttachment, part, load: .clear)
                pass.depthAttachment.clearDepth = depth
            }
            if hasStencil(part.texture.pixelFormat) {
                attach(pass.stencilAttachment, part, load: .clear)
                pass.stencilAttachment.clearStencil = stencil
            }
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
    }

    static func writes(_ gpu: GPU) throws -> MTLDepthStencilState {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .always
        d.isDepthWriteEnabled = true
        let face = MTLStencilDescriptor()
        face.depthStencilPassOperation = .replace
        d.frontFaceStencil = face
        d.backFaceStencil = face
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        return state
    }

    static func pass(_ gpu: GPU, depth: Part?, stencil: Part?, drawn: Bool, colour: MTLTexture? = nil) async throws {
        let stateFormat = { (part: Part?) -> MTLPixelFormat in
            guard let format = part?.texture.pixelFormat else { return .invalid }
            return format == .x32_stencil8 ? .depth32Float_stencil8 : format
        }
        let state =
            drawn
                ? try gpu.render(
                    fragment: colour == nil ? "zeta_no_colour" : "solid_fragment",
                    format: colour?.pixelFormat ?? .invalid,
                    depth: stateFormat(depth), stencil: stateFormat(stencil)
                ) : nil
        let written = try writes(gpu)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            if let depth {
                attach(pass.depthAttachment, depth, load: drawn ? .load : .clear)
                pass.depthAttachment.clearDepth = 0.75
            }
            if let stencil {
                attach(pass.stencilAttachment, stencil, load: drawn ? .load : .clear)
                pass.stencilAttachment.clearStencil = 9
            }
            if let colour {
                attach(pass.colorAttachments[0], Part(texture: colour), load: .clear)
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            }
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            if let state {
                encoder.setRenderPipelineState(state)
                encoder.setDepthStencilState(written)
                encoder.setStencilReferenceValue(7)
                GPU.draw(encoder, corners: GPU.fullScreen, color: SIMD4(1, 1, 1, 1))
            }
            encoder.endEncoding()
        }
    }

    static func off(_ gpu: GPU, _ check: Check) async throws -> Int {
        let t = check.part.texture
        let (level, slice) = (check.part.level, check.part.slice)
        let width = max(1, t.width >> level)
        let words: [UInt32]
        switch check.aspect {
        case .depth:
            let options: MTLBlitOption = t.pixelFormat == .depth32Float_stencil8 ? .depthFromDepthStencil : []
            let bytes = try await gpu.read(t, bytesPerPixel: 4, level: level, slice: slice, options: options)
            words = (0 ..< bytes.count / 4).map { bytes.loaded(as: UInt32.self, at: $0) }
        case .stencil:
            let options: MTLBlitOption = t.pixelFormat == .depth32Float_stencil8 ? .stencilFromDepthStencil : []
            words = try await gpu.read(t, bytesPerPixel: 1, level: level, slice: slice, options: options).map(
                UInt32.init
            )
        case .colour:
            let bytes = try await gpu.read(t, bytesPerPixel: 4, level: level, slice: slice)
            words = (0 ..< bytes.count / 4).map { bytes.loaded(as: UInt32.self, at: $0) }
        }
        return words.indices.filter { words[$0] != check.want($0 % width, $0 / width) }.count
    }

    static func twoTextures(_ gpu: GPU, drawn: Bool) async throws -> [Check] {
        let a = Part(texture: try zeta(gpu, .depth32Float_stencil8))
        let b = Part(texture: try zeta(gpu, .depth32Float_stencil8))
        try await clear(gpu, a, depth: 0.1, stencil: 1)
        try await clear(gpu, b, depth: 0.2, stencil: 2)
        try await pass(gpu, depth: a, stencil: b, drawn: drawn)
        return [
            Check(aspect: .depth, part: a) { _, _ in depth(drawn ? 0.5 : 0.75) },
            Check(aspect: .stencil, part: a) { _, _ in 1 }, Check(aspect: .depth, part: b) { _, _ in depth(0.2) },
            Check(aspect: .stencil, part: b) { _, _ in drawn ? 7 : 9 },
        ]
    }

    static func twoParts(_ gpu: GPU, slices: Bool, drawn: Bool) async throws -> [Check] {
        let t =
            slices
                ? try zeta(gpu, .depth32Float_stencil8, slices: 2)
                : try zeta(gpu, .depth32Float_stencil8, side: 64, levels: 2)
        let first = Part(texture: t)
        let second = slices ? Part(texture: t, slice: 1) : Part(texture: t, level: 1)
        try await clear(gpu, first, depth: 0.1, stencil: 1)
        try await clear(gpu, second, depth: 0.2, stencil: 2)
        try await pass(gpu, depth: first, stencil: second, drawn: drawn)
        return [
            Check(aspect: .depth, part: first) { x, y in depth(drawn ? (inner(x, y) ? 0.5 : 0.1) : 0.75) },
            Check(aspect: .stencil, part: first) { _, _ in 1 },
            Check(aspect: .depth, part: second) { _, _ in depth(0.2) },
            Check(aspect: .stencil, part: second) { _, _ in drawn ? 7 : 9 },
        ]
    }

    static func mixed(_ gpu: GPU) async throws -> [Check] {
        let a = Part(texture: try zeta(gpu, .depth32Float_stencil8)), b = Part(texture: try zeta(gpu, .stencil8))
        try await clear(gpu, a, depth: 0.1, stencil: 1)
        try await clear(gpu, b, depth: 0, stencil: 2)
        try await pass(gpu, depth: a, stencil: b, drawn: false)
        let c = Part(texture: try zeta(gpu, .depth32Float)), d = Part(texture: try zeta(gpu, .depth32Float_stencil8))
        try await clear(gpu, c, depth: 0.1, stencil: 0)
        try await clear(gpu, d, depth: 0.2, stencil: 2)
        try await pass(gpu, depth: c, stencil: d, drawn: false)
        return [
            Check(aspect: .depth, part: a) { _, _ in depth(0.75) }, Check(aspect: .stencil, part: a) { _, _ in 1 },
            Check(aspect: .stencil, part: b) { _, _ in 9 }, Check(aspect: .depth, part: c) { _, _ in depth(0.75) },
            Check(aspect: .depth, part: d) { _, _ in depth(0.2) }, Check(aspect: .stencil, part: d) { _, _ in 9 },
        ]
    }

    static func sizes(_ gpu: GPU) async throws -> [Check] {
        let d = Part(texture: try zeta(gpu, .depth32Float, side: 64)), s = Part(texture: try zeta(gpu, .stencil8))
        let colour = try gpu.texture(.rgba8Unorm, 64, 64)
        try await gpu.upload([UInt8](repeating: 0x22, count: 64 * 64 * 4), to: colour, bytesPerRow: 64 * 4)
        try await clear(gpu, d, depth: 0.1, stencil: 0)
        try await clear(gpu, s, depth: 0, stencil: 2)
        try await pass(gpu, depth: d, stencil: s, drawn: true, colour: colour)
        return [
            Check(aspect: .depth, part: d) { x, y in depth(inner(x, y) ? 0.5 : 0.1) },
            Check(aspect: .stencil, part: s) { _, _ in 7 },
            Check(aspect: .colour, part: Part(texture: colour)) { x, y in inner(x, y) ? 0xffff_ffff : 0x2222_2222 },
        ]
    }

    static func stencilView(_ gpu: GPU, alone: Bool) async throws -> [Check] {
        let a = Part(
            texture: try zeta(gpu, .depth32Float_stencil8, usage: [.renderTarget, .shaderRead, .pixelFormatView])
        )
        guard let view = a.texture.makeTextureView(pixelFormat: .x32_stencil8) else {
            throw TestFailure("no X32_Stencil8 view")
        }
        try await clear(gpu, a, depth: 0.1, stencil: 1)
        try await pass(gpu, depth: alone ? nil : a, stencil: Part(texture: view), drawn: true)
        return [
            Check(aspect: .depth, part: a) { _, _ in depth(alone ? 0.1 : 0.5) },
            Check(aspect: .stencil, part: a) { _, _ in 7 },
        ]
    }
}
