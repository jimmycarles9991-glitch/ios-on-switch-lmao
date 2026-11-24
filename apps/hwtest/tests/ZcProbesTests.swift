import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The compression and zcull cases left behind (zcull-comp LIST #18, #15, #27, #32), each run under the facade's debug
/// switch that builds it for the run (TegraGM20BMetalProbe.mm switchMetalProbeZetaLayers: .. ZcullCriterion:) beside
/// the same scene without it. They decide, not judge: every count 0 under the switch means the case can be built;
/// a count is the finding. FAIL only where the scene could not run. The wanted values are what any GPU makes of these
/// scenes (a cleared or drawn depth read back, a nearer quad over a farther one, a stencil test no fragment passes);
/// none was measured on a Mac.
enum ZcProbes {
    static let side = 128

    /// The facade's switch `name` (an NSNumber argument) set to `value` for `body`, then off; nil where the facade
    /// has no such switch
    static func switched<T>(
        _ gpu: GPU, _ name: String, _ value: NSNumber, _ body: () async throws -> T
    ) async throws -> T? {
        let selector = NSSelectorFromString(name)
        guard let facade = gpu.device as? NSObject, facade.responds(to: selector) else { return nil }
        _ = facade.perform(selector, with: value)
        defer { _ = facade.perform(selector, with: NSNumber(value: 0)) }
        return try await body()
    }

    static func depthState(_ gpu: GPU, _ compare: MTLCompareFunction, write: Bool) throws -> MTLDepthStencilState {
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = compare
        d.isDepthWriteEnabled = write
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no depth state") }
        return state
    }

    /// Words of `bytes` read as floats that are not `want(texel)`
    static func floatsOff(_ bytes: [UInt8], _ want: (Int) -> Float) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { at in
            let bits = UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16
                | UInt32(bytes[at + 3]) << 24
            return Float(bitPattern: bits) != want(at / 4)
        }.count
    }

    /// BGRA8 texels of `bytes` that are not `want`
    static func texelsOff(_ bytes: [UInt8], _ want: [UInt8]) -> Int {
        stride(from: 0, to: bytes.count, by: 4).filter { !bytes[$0 ..< $0 + 4].elementsEqual(want) }.count
    }
}

/// LIST #18: a Depth32Float 2D array of 6 layers on compressed memory (switchMetalProbeZetaLayers: waives storedZetaKind's
/// one layer), each layer cleared to (l + 1) / 8, read back layer by layer through the copy engine
/// (`layer_readback_wrong`), then layer 0 copied into layer 5 by a blit and read (`layer_copy_wrong`); the same over
/// plain memory is the control. All 0 with `zeta_layers_comptags` 1 means stored zetas of several layers can compress.
struct ZcZetaLayersTest: HWTest {
    let id = "gpu.gm20b.zc_zeta_layers"
    let name = "Layered depth probe: a compressed Depth32Float 2D array read back and copied layer by layer"
    let subsystem = Subsystem.metal

    static let layers = 6

    static func value(_ layer: Int) -> Float { Float(layer + 1) / 8 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        let scene = { () async throws -> (comptags: Int, readback: Int, copy: Int) in
            let depth = try await CompressionSettled.remade(gpu.device) { try Self.array(gpu) }
            let comptags = CompressionSettled.comptags(gpu.device, depth) ?? -1
            let readback = try await Self.readback(gpu, depth)
            let copy = try await Self.copied(gpu, depth)
            return (comptags, readback, copy)
        }
        let probed = try await ZcProbes.switched(gpu, "switchMetalProbeZetaLayers:", NSNumber(value: true), scene)
        guard let probed else { return .skip(GM20BFacade.missing("switchMetalProbeZetaLayers:", on: gpu.device)) }
        let control = try await scene()
        for (prefix, r) in [("", probed), ("control_", control)] {
            metrics.set(prefix + "zeta_layers_comptags", r.comptags)
            metrics.set(prefix + "layer_readback_wrong", r.readback)
            metrics.set(prefix + "layer_copy_wrong", r.copy)
        }
        return Outcome.pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func array(_ gpu: GPU) throws -> MTLTexture {
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .depth32Float, width: ZcProbes.side, height: ZcProbes.side, mipmapped: false
        )
        d.textureType = .type2DArray
        d.arrayLength = layers
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no Depth32Float 2D array") }
        return t
    }

    static func readback(_ gpu: GPU, _ depth: MTLTexture) async throws -> Int {
        for layer in 0 ..< layers {
            try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.depthAttachment.texture = depth
                pass.depthAttachment.slice = layer
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = Double(value(layer))
                pass.depthAttachment.storeAction = .store
                pass.renderTargetWidth = ZcProbes.side
                pass.renderTargetHeight = ZcProbes.side
                commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            }
        }
        var off = 0
        for layer in 0 ..< layers {
            let bytes = try await gpu.read(depth, bytesPerPixel: 4, slice: layer)
            off += ZcProbes.floatsOff(bytes) { _ in value(layer) }
        }
        return off
    }

    static func copied(_ gpu: GPU, _ depth: MTLTexture) async throws -> Int {
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: depth, sourceSlice: 0, sourceLevel: 0, to: depth, destinationSlice: layers - 1,
                destinationLevel: 0, sliceCount: 1, levelCount: 1
            )
            blit.endEncoding()
        }
        let bytes = try await gpu.read(depth, bytesPerPixel: 4, slice: layers - 1)
        return ZcProbes.floatsOff(bytes) { _ in value(0) }
    }
}

/// LIST #15: a pass whose depth attachment is an R32Float colour texture (rendered through a zeta shadow), its shadow
/// backed by compressed memory under switchMetalProbeZetaShadow: (the shadow of a compressed stored zeta, which the
/// switch also copies without its decompress, needs a layered pass and is not drawn here): cleared to 0.5, a quad over
/// the left half at 0.25 under Less with writes, read back (`shadow_wrong`: texels off 0.25 left, 0.5 right); the
/// control without the switch (`control_shadow_wrong`).
struct ZcZetaShadowTest: HWTest {
    let id = "gpu.gm20b.zc_zeta_shadow"
    let name = "Zeta shadow probe: an R32Float texture as depth through a compressed shadow backing"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        let probed = try await ZcProbes.switched(gpu, "switchMetalProbeZetaShadow:", NSNumber(value: true)) {
            try await Self.scene(gpu)
        }
        guard let probed else { return .skip(GM20BFacade.missing("switchMetalProbeZetaShadow:", on: gpu.device)) }
        let control = try await Self.scene(gpu)
        metrics.set("shadow_wrong", probed)
        metrics.set("control_shadow_wrong", control)
        return Outcome.pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func scene(_ gpu: GPU) async throws -> Int {
        let side = ZcProbes.side
        let depth = try gpu.texture(.r32Float, side, side), colour = try gpu.texture(.bgra8Unorm, side, side)
        let pipeline = try gpu.render(fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float)
        let less = try ZcProbes.depthState(gpu, .less, write: true)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = depth
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.5
            pass.depthAttachment.storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.setDepthStencilState(less)
            GPU.draw(
                e, corners: GPU.pixelQuad(0, 0, side / 2, side, width: side, height: side, z: 0.25),
                color: SIMD4(1, 1, 1, 1)
            )
            e.endEncoding()
        }
        let bytes = try await gpu.read(depth, bytesPerPixel: 4)
        return ZcProbes.floatsOff(bytes) { $0 % side < side / 2 ? 0.25 : 0.5 }
    }
}

/// LIST #27: which zcull a mixed pass takes (TirRows): boot 2's region over the render area's pixels, written before
/// the clears, died at the first draw with INVALID_TIR_COMBINATION05. Rows z_<region>_<order>_<mode>: the region over
/// the pixels (px) or the zeta's 2x2 sample grid (grid), its words before the clears (before) or after the mode's
/// enable (after), the mode on (on) or left disabled (off); z_none and z_none_off take no region. An accepted row with
/// _wrong 0 is a zcull a mixed pass can take; mixed passes keep zcull off until one is.
struct ZcMixedTest: HWTest {
    let id = "gpu.gm20b.zc_mixed"
    let name = "Mixed-sample zcull probe: which region, order and mode a 1x colour beside a 4x depth takes"
    let subsystem = Subsystem.metal

    static let rows: [TirRows.Row] = [
        TirRows.Row(name: "z_none", word: 1), TirRows.Row(name: "z_none_off", word: 1 | 16),
    ] + [false, true].flatMap { off in
        [false, true].flatMap { after in
            [false, true].map { grid in
                TirRows.Row(
                    name: "z_\(grid ? "grid" : "px")_\(after ? "after" : "before")_\(off ? "off" : "on")",
                    word: 1 | 2 | (grid ? 4 : 0) | (after ? 8 : 0) | (off ? 16 : 0)
                )
            }
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        guard try await TirRows.run(gpu, Self.rows, &metrics) != nil else {
            return .skip(GM20BFacade.missing("switchMetalProbeTir:", on: gpu.device))
        }
        return Outcome.pass(metrics, proof: gpu.proof)
    }
}

/// LIST #32: SET_ZCULL_CRITERION SFUNC EQUAL, SREF 1, SMASK 0xff on the region (switchMetalProbeZcullCriterion:) of a
/// pass over a memoryless Depth32Float_Stencil8 cleared to depth 1 and stencil 0, 64 full quads under stencil Equal 1
/// (none passes) and depth Less; `stencil_cull_wrong` texels the quads changed (the clear's black kept everywhere),
/// `stencil_cull_gpu_ms` against `stencil_never_gpu_ms` under NVK's NEVER. A shorter time with nothing wrong means
/// zcull culls by the stencil criterion.
struct ZcStencilCriterionTest: HWTest {
    let id = "gpu.gm20b.zc_stencil_criterion"
    let name = "Zcull stencil criterion probe: Equal 1 over a stencil cleared to 0"
    let subsystem = Subsystem.metal

    static let side = 512, draws = 64
    /// SFUNC EQUAL (2) in byte 0, SREF 1 in byte 1, SMASK 0xff in byte 2
    static let criterion: UInt32 = 2 | 1 << 8 | 0xFF << 16

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        let probed = try await ZcProbes.switched(
            gpu, "switchMetalProbeZcullCriterion:", NSNumber(value: Self.criterion)
        ) { try await Self.scene(gpu) }
        guard let probed else { return .skip(GM20BFacade.missing("switchMetalProbeZcullCriterion:", on: gpu.device)) }
        let never = try await Self.scene(gpu)
        metrics.set("stencil_cull_wrong", probed.wrong)
        metrics.set("stencil_cull_gpu_ms", probed.ms, digits: 3)
        metrics.set("stencil_never_wrong", never.wrong)
        metrics.set("stencil_never_gpu_ms", never.ms, digits: 3)
        return Outcome.pass(metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func scene(_ gpu: GPU) async throws -> (wrong: Int, ms: Double) {
        let colour = try gpu.texture(.bgra8Unorm, side, side)
        let zeta = try gpu.texture(.depth32Float_stencil8, side, side, usage: [.renderTarget], storage: .memoryless)
        let pipeline = try gpu.render(
            fragment: "exact_fragment", format: .bgra8Unorm, depth: .depth32Float_stencil8,
            stencil: .depth32Float_stencil8
        )
        let d = MTLDepthStencilDescriptor()
        d.depthCompareFunction = .less
        d.isDepthWriteEnabled = true
        let stencil = MTLStencilDescriptor()
        stencil.stencilCompareFunction = .equal
        d.frontFaceStencil = stencil
        d.backFaceStencil = stencil
        guard let state = gpu.device.makeDepthStencilState(descriptor: d) else { throw TestFailure("no stencil state") }
        let ms = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = colour
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            pass.depthAttachment.texture = zeta
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 1
            pass.depthAttachment.storeAction = .dontCare
            pass.stencilAttachment.texture = zeta
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0
            pass.stencilAttachment.storeAction = .dontCare
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            e.setDepthStencilState(state)
            e.setStencilReferenceValue(1)
            for _ in 0 ..< draws {
                GPU.draw(
                    e, corners: GPU.pixelQuad(0, 0, side, side, width: side, height: side, z: 0.5),
                    color: SIMD4(1, 1, 1, 1)
                )
            }
            e.endEncoding()
        }.gpuMilliseconds
        let bytes = try await gpu.read(colour, bytesPerPixel: 4)
        return (ZcProbes.texelsOff(bytes, [0, 0, 0, 255]), ms)
    }
}
