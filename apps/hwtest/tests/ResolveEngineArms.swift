import Foundation
import HWTestCore
import HWTestGPU
import HWTestX1
import Metal

/// gpu.gm20b.resolve_twod's 2D-engine arms beyond the rule's routes, each held to Metal's rule as the format arms are
/// (ResolveFormatArms.swift `offMetal`): `<route or engine work>:<grade>:<texels off Metal>:<channels beyond one>`.
/// - `<cell>_engine`: a cell the rule sends to the pack's pass (sRGB at 4x) resolved by the engine anyway, under the
///   probe form `resolve=twod` (probe_forms.h), counted by the command buffer's probe resolves. FAIL: none taken.
/// - `<cell>_via16f`: an sRGB cell's samples carried as linear light into an RGBA16Float grid of the same count,
///   that grid resolved into a private RGBA16Float intermediate only a pass reads (resolve.twod), and the pass
///   storing it into the sRGB format; `<cell>_via16f_pass` the same with the intermediate resolved by the pack's
///   pass, its control. FAIL: the intermediate's resolve not taken by the engine.
/// - `<cell>_pitch` (RGBA16Float at 2x, the exact cell): the resolve into a pitch texture over a shared buffer, a
///   target read as data, which only an exact cell sends to the engine. FAIL: not the engine, or a texel off Metal.
/// - `<cell>_pitchgrid` (the same cell): the samples copied side by side into a pitch grid over a shared buffer (the
///   2x grid's own layout, 2x1 samples a pixel) and that grid boxed by the engine as resolve_twod.cpp boxes the
///   block-linear one (probe_ops.h probeResolveOp, at an open blit encoder), counted by the probe's ops: the engine's
///   half box with no block-linear read. FAIL: no op taken.
/// The engine, via16f and pitchgrid arms read work the rule does not route: their texels off Metal are no failure.
/// An arm with a channel beyond one code adds `<arm>_beyond_texels` (BeyondOneTexels) to the report.
extension ResolveTwodTest {
    static let formsSelector = NSSelectorFromString("switchMetalProbeForms:")
    static let takenSelector = NSSelectorFromString("switchMetalProbeTaken")
    static let engineSelector = NSSelectorFromString("switchMetalProbeEngine:")

    /// One format cell's scene: its pipeline and the codes each sample stores
    struct ArmScene {
        let cell: FormatCell
        let gpu: GPU
        let state: MTLRenderPipelineState
        let stored: [[UInt8]]

        var format: X1Format { cell.format }

        /// `<key>=<field>:<grade>:<off>:<beyond>` and its texels beyond one code
        func judge(_ key: String, _ field: String, _ bytes: [UInt8], _ metrics: inout Metrics) -> Int {
            let (off, beyond) = ResolveTwodTest.offMetal(format, bytes, stored)
            metrics.set(key, "\(field):\(ResolveTwodTest.grade(off, beyond)):\(off):\(beyond)")
            ResolveTwodTest.noteBeyond(key, ResolveTwodTest.beyondTexels(format, bytes, stored), beyond, &metrics)
            return off
        }
    }

    func engineArms(_ scene: ArmScene, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String]) async {
        let cell = scene.cell
        let arms: [(String, Bool)] = [
            ("engine", !cell.twod), ("via16f", cell.format.channels.contains(.srgb8)), ("pitch", cell.exact),
            ("pitchgrid", cell.exact && cell.samples == 2),
        ]
        for (arm, runs) in arms where runs {
            do {
                switch arm {
                case "engine": try await engineArm(scene, &metrics, &wrong)
                case "via16f": try await linearArm(scene, facade, &metrics, &wrong)
                case "pitch": try await pitchArm(scene, &metrics, &wrong)
                default: try await pitchGridArm(scene, &metrics, &wrong)
                }
            } catch {
                metrics.set("\(cell.name)_\(arm)_error", String(describing: error).prefix(80).description)
                wrong.append("\(cell.name)_\(arm)_error")
            }
        }
    }

    static func grade(_ off: Int, _ beyond: Int) -> String { off == 0 ? "exact" : beyond == 0 ? "near" : "wrong" }

    /// Every third texel each way whose codes are more than one code from Metal's resolve of the stored samples
    static func beyondTexels(_ format: X1Format, _ got: [UInt8], _ stored: [[UInt8]]) -> BeyondOneTexels {
        var out = BeyondOneTexels()
        for y in stride(from: 0, to: height, by: 3) {
            for x in stride(from: 0, to: width, by: 3) {
                let t = y * width + x
                let samples = stored.map { format.codes($0, t) }
                out.add(x: x, y: y, got: format.codes(got, t), want: metalResolve(format, samples), samples: samples)
            }
        }
        return out
    }

    /// `<key>_beyond_texels` in the report alone, where a channel is beyond one code
    static func noteBeyond(
        _ key: String, _ texels: @autoclosure () -> BeyondOneTexels, _ beyond: Int, _ metrics: inout Metrics
    ) {
        guard beyond > 0 else { return }
        metrics.set(key + "_beyond_texels", texels().text)
        metrics.keepInReportOnly(key + "_beyond_texels")
    }

    func engineArm(_ scene: ArmScene, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let (gpu, format) = (scene.gpu, scene.format)
        let ms = try gpu.texture(
            format.pixelFormat, Self.width, Self.height, usage: [.renderTarget], samples: scene.cell.samples
        )
        let into = try gpu.texture(format.pixelFormat, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        var buffer: MTLCommandBuffer?
        let timing = try await gpu.run { commands in
            buffer = commands
            try Self.probeForms(commands, "resolve=twod")
            draw(commands, pass(ms, into: into, load: .clear, store: .multisampleResolve), scene.state)
        }
        let resolves = buffer.map { Self.probeTaken($0, "resolves") } ?? 0
        let key = scene.cell.name + "_engine"
        let bytes = try await gpu.read(into, bytesPerPixel: format.bytesPerTexel)
        _ = scene.judge(key, String(resolves), bytes, &metrics)
        metrics.set(key + "_ms", timing.gpuMilliseconds, digits: 3)
        if resolves == 0 { wrong.append(key + "_not_taken") }
    }

    func linearArm(_ scene: ArmScene, _ facade: NSObject, _ metrics: inout Metrics, _ wrong: inout [String])
        async throws
    {
        let name = scene.cell.name
        let engine = try await viaHalfGrid(scene)
        let control = try await switched(facade, Self.drawSelector) { try await viaHalfGrid(scene) }
        for (key, read) in [(name + "_via16f", engine), (name + "_via16f_pass", control)] {
            _ = scene.judge(key, read.twod > 0 ? "twod" : "pass", read.bytes, &metrics)
        }
        metrics.set(name + "_via16f_ms", String(format: "%.3f/%.3f", engine.gpuMs, control.gpuMs))
        if engine.twod == 0 { wrong.append(name + "_via16f_not_twod") }
    }

    /// The cell's samples drawn, carried into an RGBA16Float grid, resolved into a private RGBA16Float texture, and
    /// that stored into the cell's format by a pass, in one command buffer; `twod` counts the engine's resolves in it
    func viaHalfGrid(_ scene: ArmScene) async throws -> Resolved {
        let (gpu, w, h, n) = (scene.gpu, Self.width, Self.height, scene.cell.samples)
        let format = scene.format.pixelFormat
        let grid = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead], samples: n)
        let halfGrid = try gpu.texture(.rgba16Float, w, h, usage: [.renderTarget], samples: n)
        let half = try gpu.texture(.rgba16Float, w, h, usage: [.renderTarget, .shaderRead])
        let out = try gpu.texture(format, w, h, usage: [.renderTarget, .shaderRead])
        let widen = try gpu.render(fragment: "resolve_twod_linear_sample", format: .rgba16Float, samples: n)
        let store = try gpu.render(fragment: "texel_read", format: format)
        let before = GM20BFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            draw(commands, Self.storePass(grid, load: .clear), scene.state)
            let carried = pass(halfGrid, into: half, load: .dontCare, store: .multisampleResolve)
            Self.drawReading(commands, carried, widen, grid)
            Self.drawReading(commands, Self.storePass(out, load: .dontCare), store, half)
        }
        let after = GM20BFacade.counters(of: gpu.device)
        var read = Resolved()
        read.gpuMs = timing.gpuMilliseconds
        read.path = timing.path
        read.twod = (after["resolve_twod"] ?? 0) - (before["resolve_twod"] ?? 0)
        read.bytes = try await gpu.read(out, bytesPerPixel: scene.format.bytesPerTexel)
        return read
    }

    func pitchArm(_ scene: ArmScene, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let (gpu, format) = (scene.gpu, scene.format)
        let into = try Self.pitchTexture(gpu, format, Self.width, Self.height)
        let ms = try gpu.texture(
            format.pixelFormat, Self.width, Self.height, usage: [.renderTarget], samples: scene.cell.samples
        )
        let before = GM20BFacade.counters(of: gpu.device)
        try await gpu.run { commands in
            draw(commands, pass(ms, into: into.texture, load: .clear, store: .multisampleResolve), scene.state)
        }
        let after = GM20BFacade.counters(of: gpu.device)
        let route = (after["resolve_twod"] ?? 0) > (before["resolve_twod"] ?? 0) ? "twod" : "pass"
        let key = scene.cell.name + "_pitch"
        let off = scene.judge(key, route, into.bytes(), &metrics)
        if route != "twod" { wrong.append(key + "_not_twod") }
        if off > 0 { wrong.append(key + "_off_metal") }
    }

    func pitchGridArm(_ scene: ArmScene, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let (gpu, format, n) = (scene.gpu, scene.format, scene.cell.samples)
        let ms = try gpu.texture(
            format.pixelFormat, Self.width, Self.height, usage: [.renderTarget, .shaderRead], samples: n
        )
        let grid = try Self.pitchTexture(gpu, format, Self.width * n, Self.height)
        let into = try gpu.texture(format.pixelFormat, Self.width, Self.height, usage: [.renderTarget, .shaderRead])
        let spread = try gpu.render(fragment: "ms_sample_fragment", format: format.pixelFormat)
        let du = UInt64(n) << 32, dv = UInt64(1) << 32
        let op: [String: Any] = [
            "kind": "scale", "read": grid.texture, "written": into, "du": du, "dv": dv, "sx": du / 2, "sy": dv / 2,
            "bilinear": 1, "corner": 1,
        ]
        var buffer: MTLCommandBuffer?
        try await gpu.run { commands in
            buffer = commands
            draw(commands, Self.storePass(ms, load: .clear), scene.state)
            let e = commands.makeRenderCommandEncoder(descriptor: Self.storePass(grid.texture, load: .dontCare))!
            e.setRenderPipelineState(spread)
            var corners = GPU.fullScreen, count = UInt32(n)
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setFragmentTexture(ms, index: 0)
            e.setFragmentBytes(&count, length: 4, index: 0)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            e.endEncoding()
            let blit = commands.makeBlitCommandEncoder()!
            try Self.probeCall(commands, Self.engineSelector, op as NSDictionary)
            blit.endEncoding()
        }
        let ops = buffer.map { Self.probeTaken($0, "ops") } ?? 0
        let key = scene.cell.name + "_pitchgrid"
        let bytes = try await gpu.read(into, bytesPerPixel: format.bytesPerTexel)
        _ = scene.judge(key, String(ops), bytes, &metrics)
        if ops == 0 { wrong.append(key + "_not_taken") }
    }

    static func storePass(_ texture: MTLTexture, load: MTLLoadAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = texture
        pass.colorAttachments[0].loadAction = load
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    static func drawReading(
        _ commands: MTLCommandBuffer, _ pass: MTLRenderPassDescriptor, _ state: MTLRenderPipelineState,
        _ texture: MTLTexture
    ) {
        let e = commands.makeRenderCommandEncoder(descriptor: pass)!
        e.setRenderPipelineState(state)
        var corners = GPU.fullScreen
        e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        e.setFragmentTexture(texture, index: 0)
        e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        e.endEncoding()
    }

    /// A pitch texture over a shared buffer, rows a multiple of 256 bytes, so GM20B renders it in place (linearTarget)
    struct PitchTexture {
        let texture: MTLTexture
        let backing: MTLBuffer
        let row: Int
        let packed: Int

        func bytes() -> [UInt8] {
            let rows = UnsafeRawBufferPointer(start: backing.contents(), count: row * texture.height)
            return (0 ..< texture.height).flatMap { rows[$0 * row ..< $0 * row + packed] }
        }
    }

    static func pitchTexture(_ gpu: GPU, _ format: X1Format, _ width: Int, _ height: Int) throws -> PitchTexture {
        let packed = width * format.bytesPerTexel
        let alignment = max(256, gpu.device.minimumLinearTextureAlignment(for: format.pixelFormat))
        let row = (packed + alignment - 1) / alignment * alignment
        let backing = try gpu.buffer(row * height)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format.pixelFormat, width: width, height: height, mipmapped: false
        )
        descriptor.storageMode = .shared
        descriptor.usage = [.renderTarget, .shaderRead]
        guard let texture = backing.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: row) else {
            throw TestFailure("a pitch \(format.name) texture over a shared buffer was not made")
        }
        return PitchTexture(texture: texture, backing: backing, row: row, packed: packed)
    }

    /// probe_forms.h's forms for `commands`, before it commits (debug builds' TegraGM20BMetalProbe.mm)
    static func probeForms(_ commands: MTLCommandBuffer, _ forms: String) throws {
        try probeCall(commands, formsSelector, forms)
    }

    /// A debug-build probe call on `commands` whose answer is nil, else why not
    static func probeCall(_ commands: MTLCommandBuffer, _ selector: Selector, _ argument: Any) throws {
        guard let object = commands as? NSObject, object.responds(to: selector) else {
            throw TestFailure("the facade has no \(NSStringFromSelector(selector)) (a release plug-in)")
        }
        if let refusal = object.perform(selector, with: argument)?.takeUnretainedValue() as? String {
            throw TestFailure(refusal)
        }
    }

    /// One field of where the probe forms and ops took effect once `commands` completed (probe_forms.h ProbeTaken)
    static func probeTaken(_ commands: MTLCommandBuffer, _ field: String) -> Int {
        guard let object = commands as? NSObject, object.responds(to: takenSelector),
              let line = object.perform(takenSelector)?.takeUnretainedValue() as? String
        else { return 0 }
        let fields = line.split(separator: " ").map { $0.split(separator: "=", maxSplits: 1) }
        return fields.first { $0.count == 2 && $0[0] == field }.flatMap { Int($0[1]) } ?? 0
    }
}
