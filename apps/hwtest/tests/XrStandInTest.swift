import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// BGRA10_XR (552), BGR10_XR (554) and their sRGB twins (553, 555) render into an RGBA16Float stand-in that holds
/// Metal's tile, against an M4 Max's codes (macOS 27.2.1, alike on descriptor and surface textures). tile: a 4x4
/// target, one pass of an unblended full draw then a One/One one (alpha 0.25 each, or 1.5 then -1.0); <form>_<case>_r
/// and _a its stored red and alpha codes, <form>_<case>_metal Metal's. Judged equal to Metal's: hold (two +0.3 codes
/// store +1), read_low (-2.0 unblended, then +1.0: the blend reads the destination at the source range's low end),
/// source (a blended source is clamped to the source range: code 1022, 555's 947), overbright (an unclamped sum stores
/// 1023). Reported: read_high (2.0 unblended, then -1.0: Metal's blend reads the destination clamped to the source
/// range's high end, 638, 555's 758; the stand-in holds code 1023's value, which a lone unblended write stores, and the
/// ROP reads it unclamped: 639, 555's about 876, the overbright-reblend case that reaches 376 codes on 555), back (1.2,
/// +0.5, -0.5: Metal's third draw reads the sum clamped) and loaded (codes 1023/0/1023/1023 loaded, +0 blended: Metal
/// stores 1022/0/1022/894, 555's 947; the stand-in keeps a texel left at its load's value). keeps: a 256x160 texture of
/// codes 1023/500/100/0 with padding bits set, a Load pass of one unblended 1x1 draw of 0.5 at (0, 0). Metal rewrites
/// the whole 32x32 tile the draw reached (1024 texels, alpha clamped, padding cleared); GM20B keeps no record of the
/// tiles a pass's fragments reach, so the stand-in rewrites the drawn texel alone: <form>_keep_changed (judged 1),
/// <form>_keep_drawn (judged 0.5's codes: 639, the sRGB forms' colour 759). Every command buffer raw, none failed.
struct XrStandInTest: HWTest {
    let id = "gpu.gm20b.xr_stand_in"
    let name = "Extended-range passes hold Metal's tile in a half stand-in and keep texels no draw changed"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Draw = (state: MTLRenderPipelineState, corners: [SIMD4<Float>], colour: SIMD4<Float>)

    struct Form {
        let name: String
        let layout: XrSrgbFormat
        /// Metal's stored red and alpha codes of each case, in `cases`' order
        let metal: [(red: Int, alpha: Int)]
        /// The colour code of 0.5
        let half: Int
    }

    struct Case {
        let name: String
        let first: Float, second: Float
        let judged: Bool
    }

    static let forms = [
        Form(
            name: "bgra10", layout: XrSrgbFormat(format: .bgra10_xr, bytes: 8, alpha: true),
            metal: [(301, 639), (638, 894), (510, 639), (1022, 639), (1023, 639), (767, 894)], half: 639
        ),
        Form(
            name: "bgr10", layout: XrSrgbFormat(format: .bgr10_xr, bytes: 4, alpha: false),
            metal: [(301, 0), (638, 0), (510, 0), (1022, 0), (1023, 0), (767, 0)], half: 639
        ),
        Form(
            name: "bgra10_srgb", layout: .bgra,
            metal: [(159, 639), (925, 894), (750, 639), (1022, 639), (1023, 639), (930, 894)], half: 759
        ),
        Form(
            name: "bgr10_srgb", layout: .bgr,
            metal: [(159, 0), (758, 0), (656, 0), (947, 0), (1023, 0), (833, 0)], half: 759
        ),
    ]

    static let c300 = Float((300.0 - 384.0) / 510.0)
    static let cases = [
        Case(
            name: "hold", first: Float(Float16(c300 + 0.3 / 510)), second: Float(Float16(Float(0.3 / 510))),
            judged: true
        ),
        Case(name: "read_high", first: 2, second: -1, judged: false),
        Case(name: "read_low", first: -2, second: 1, judged: true),
        Case(name: "source", first: 0, second: 2, judged: true),
        Case(name: "overbright", first: 1, second: 1, judged: true),
        Case(name: "back", first: 1.2, second: 0.5, judged: false),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for form in Self.forms {
            do {
                try await tile(gpu, form, &metrics, &checks)
                try await loaded(gpu, form, &metrics)
                try await keeps(gpu, form, &metrics, &checks)
            } catch {
                checks.append(("\(form.name): \(GPU.describe(error))", false))
            }
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(
            within: 720, keeping: ["paths", "failed_cbs", "bgr10_srgb_read_high_r", "bgr10_srgb_read_high_metal"]
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }

    struct States {
        let plain: MTLRenderPipelineState
        let add: MTLRenderPipelineState
    }

    func states(_ gpu: GPU, _ format: MTLPixelFormat) throws -> States {
        let plain = try gpu.render(fragment: "solid_fragment", format: format)
        let add = try gpu.render(fragment: "solid_fragment", format: format) {
            let a = $0.colorAttachments[0]!
            a.isBlendingEnabled = true
            a.sourceRGBBlendFactor = .one
            a.destinationRGBBlendFactor = .one
            a.sourceAlphaBlendFactor = .one
            a.destinationAlphaBlendFactor = .one
        }
        return States(plain: plain, add: add)
    }

    func pass(_ gpu: GPU, _ target: MTLTexture, load: MTLLoadAction, _ draws: [Draw]) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = load
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            for draw in draws {
                encoder.setRenderPipelineState(draw.state)
                GPU.draw(encoder, corners: draw.corners, color: draw.colour)
            }
            encoder.endEncoding()
        }
    }

    func tile(_ gpu: GPU, _ form: Form, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let f = form.layout
        let made = try states(gpu, f.format)
        let (plain, add) = (made.plain, made.add)
        for (c, metal) in zip(Self.cases, form.metal) {
            let target = try gpu.texture(f.format, 4, 4)
            let high = c.first > 1
            var draws: [Draw] = [
                (plain, GPU.fullScreen, SIMD4(c.first, c.first, c.first, high ? 1.5 : 0.25)),
                (add, GPU.fullScreen, SIMD4(c.second, c.second, c.second, high ? -1 : 0.25)),
            ]
            if c.name == "back" { draws.append((add, GPU.fullScreen, SIMD4<Float>(-0.5, -0.5, -0.5, 0))) }
            try await pass(gpu, target, load: .dontCare, draws)
            let codes = f.codes(try await gpu.read(target, bytesPerPixel: f.bytes))[0]
            let key = "\(form.name)_\(c.name)"
            metrics.set(key + "_r", codes[0])
            if f.alpha { metrics.set(key + "_a", codes[3]) }
            metrics.set(key + "_metal", f.alpha ? "\(metal.red)/\(metal.alpha)" : "\(metal.red)")
            guard c.judged else { continue }
            let same = codes[0] == metal.red && (!f.alpha || codes[3] == metal.alpha)
            checks.append(("\(key): Metal's \(metal.red)\(f.alpha ? "/\(metal.alpha)" : "")", same))
        }
    }

    func loaded(_ gpu: GPU, _ form: Form, _ metrics: inout Metrics) async throws {
        let f = form.layout
        let target = try gpu.texture(f.format, 4, 4, usage: [.renderTarget, .shaderRead, .shaderWrite])
        let texel = f.texel([1023, 0, 1023, 1023])
        try await gpu.upload((0 ..< 16).flatMap { _ in texel }, to: target, bytesPerRow: 4 * f.bytes)
        let add = try states(gpu, f.format).add
        try await pass(gpu, target, load: .load, [(add, GPU.fullScreen, SIMD4<Float>(0, 0, 0, 0))])
        let codes = f.codes(try await gpu.read(target, bytesPerPixel: f.bytes))[0]
        metrics.set("\(form.name)_loaded", codes.map(String.init).joined(separator: "/"))
    }

    func keeps(_ gpu: GPU, _ form: Form, _ metrics: inout Metrics, _ checks: inout [(String, Bool)]) async throws {
        let f = form.layout
        let (width, height) = (256, 160)
        let target = try gpu.texture(f.format, width, height, usage: [.renderTarget, .shaderRead, .shaderWrite])
        var texel = f.texel([1023, 500, 100, 0], pad: 1)
        if f.alpha { for k in stride(from: 0, to: 8, by: 2) { texel[k] |= 0x15 } }
        let upload = (0 ..< (width * height)).flatMap { _ in texel }
        try await gpu.upload(upload, to: target, bytesPerRow: width * f.bytes)
        let corner = GPU.pixelQuad(0, 0, 1, 1, width: width, height: height)
        let plain = try states(gpu, f.format).plain
        try await pass(gpu, target, load: .load, [(plain, corner, SIMD4<Float>(repeating: 0.5))])
        let got = try await gpu.read(target, bytesPerPixel: f.bytes)
        let changed = stride(from: 0, to: got.count, by: f.bytes).filter {
            got[$0 ..< $0 + f.bytes] != upload[$0 ..< $0 + f.bytes]
        }.count
        let drawn = f.codes(Array(got[0 ..< f.bytes]))[0]
        metrics.set("\(form.name)_keep_changed", changed)
        metrics.set("\(form.name)_keep_drawn", drawn.map(String.init).joined(separator: "/"))
        checks.append(("\(form.name): the drawn texel alone changed (Metal: its 32x32 tile)", changed == 1))
        let want = f.alpha ? [form.half, form.half, form.half, 639] : [form.half, form.half, form.half]
        checks.append(("\(form.name): the drawn texel holds 0.5's codes", drawn == want))
    }
}
