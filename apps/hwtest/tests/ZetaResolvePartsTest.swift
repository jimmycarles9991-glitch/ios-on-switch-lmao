import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Depth and stencil resolves into a part or another format: a 4x 32x32 depth drawn 0.2, 0.3, 0.4, 0.5 in samples 0-3
/// (zeta_depth_by_sample), stencil Replace 7, MultisampleResolve with filter Max unless named. a: into level 1 of a
/// 64x64 two-level Depth32Float of .9: level 1 .5, level 0 .9. b: into slice 1 of a two-slice array of .9: slice 1 .5,
/// slice 0 .9. c: a 4x Depth32Float_Stencil8's stencil, then its depth (Min), into slice 1 of a Depth32Float_Stencil8
/// array of .9/2: slice 1 .2/7, slice 0 .9/2. d: a 4x Depth32Float_Stencil8's depth into a Depth32Float (.9): .5; a 4x
/// Depth32Float into a Depth32Float_Stencil8 (.9/2): .5/2; a 4x Depth32Float_Stencil8's stencil into a Stencil8 (2): 7.
/// memoryless: a 4x Depth32Float into a memoryless Depth32Float, and a 4x Stencil8 into a memoryless Stencil8: each
/// command buffer fails MTLCommandBufferErrorInvalidResource. Tolerance 0. The expectations are an M4 Max's
/// (m2v-passes3 probe/p3probe.mm zeta_resolves, out2.txt: status 5 error 9 for memoryless; this scene on an M4 Max: 0
/// off in every case, memoryless_status 9/9). <case>_off texels off, memoryless_status the two error codes; every
/// other command buffer raw, and only the two memoryless ones failed, each the facade's pass.ds (failed_why=).
struct ZetaResolvePartsTest: HWTest {
    let id = "gpu.gm20b.zeta_resolve_parts"
    let name = "Depth and stencil resolves into a level, a slice or another format, and none into memoryless, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Part = ZetaApartTest.Part
    typealias Check = ZetaApartTest.Check

    static let side = 32
    static let depths: [Float] = [0.2, 0.3, 0.4, 0.5]
    static let cases: [(key: String, run: (GPU) async throws -> [Check])] = [
        ("a", intoLevel), ("b", intoSlice), ("c", bothIntoSlice), ("d", otherFormats),
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
                var off = 0
                for check in try await c.run(gpu) { off += try await ZetaApartTest.off(gpu, check) }
                metrics.set(c.key + "_off", off)
                checks.append(("\(c.key): each resolve in its part, nothing else touched", off == 0))
            } catch {
                checks.append(("\(c.key): \(GPU.describe(error))", false))
            }
        }
        do {
            let codes = try Self.memoryless(gpu)
            let invalid = MTLCommandBufferError.invalidResource.rawValue
            metrics.set("memoryless_status", codes.map(String.init).joined(separator: "/"))
            checks.append(("memoryless: each command buffer fails InvalidResource", codes.allSatisfy { $0 == invalid }))
        } catch {
            checks.append(("memoryless: \(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(
            FailedCounts.judged(before: before, after: counted, expecting: ["pass.ds": 2], own: gpu.paths, &metrics)
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func multisampled(_ gpu: GPU, _ format: MTLPixelFormat) throws -> MTLTexture {
        try gpu.texture(format, side, side, usage: [.renderTarget], samples: 4)
    }

    static func encodeResolve(
        _ commands: MTLCommandBuffer, from ms: MTLTexture, stencil: Bool, into part: Part,
        filter: MTLMultisampleDepthResolveFilter, state: MTLRenderPipelineState, written: MTLDepthStencilState
    ) {
        let pass = MTLRenderPassDescriptor()
        let resolved: MTLRenderPassAttachmentDescriptor = stencil ? pass.stencilAttachment : pass.depthAttachment
        resolved.texture = ms
        resolved.loadAction = .clear
        resolved.storeAction = .multisampleResolve
        resolved.resolveTexture = part.texture
        resolved.resolveLevel = part.level
        resolved.resolveSlice = part.slice
        pass.depthAttachment.depthResolveFilter = filter
        pass.depthAttachment.clearDepth = 0.75
        pass.stencilAttachment.clearStencil = 9
        if ms.pixelFormat == .depth32Float_stencil8 {
            let other: MTLRenderPassAttachmentDescriptor = stencil ? pass.depthAttachment : pass.stencilAttachment
            other.texture = ms
            other.loadAction = .clear
            other.storeAction = .dontCare
        }
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        encoder.setDepthStencilState(written)
        encoder.setStencilReferenceValue(7)
        var corners = GPU.fullScreen
        var values = depths
        encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
        encoder.setFragmentBytes(&values, length: MemoryLayout<Float>.stride * 4, index: 0)
        encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
        encoder.endEncoding()
    }

    static func state(_ gpu: GPU, _ format: MTLPixelFormat) throws -> MTLRenderPipelineState {
        let depth = ZetaApartTest.hasDepth(format)
        return try gpu.render(
            fragment: depth ? "zeta_depth_by_sample" : "zeta_no_colour", format: .invalid, samples: 4,
            depth: depth ? format : .invalid, stencil: ZetaApartTest.hasStencil(format) ? format : .invalid
        )
    }

    static func resolve(
        _ gpu: GPU, _ format: MTLPixelFormat, stencil: Bool = false, into part: Part,
        filter: MTLMultisampleDepthResolveFilter = .max
    ) async throws {
        let ms = try multisampled(gpu, format)
        let state = try state(gpu, format)
        let written = try ZetaApartTest.writes(gpu)
        try await gpu.run { commands in
            encodeResolve(
                commands, from: ms, stencil: stencil, into: part, filter: filter, state: state, written: written
            )
        }
    }

    static func into(
        _ gpu: GPU, _ format: MTLPixelFormat, side: Int = side, levels: Int = 1, slices: Int = 1
    ) async throws -> MTLTexture {
        let t = try ZetaApartTest.zeta(gpu, format, side: side, levels: levels, slices: slices)
        for level in 0 ..< levels {
            for slice in 0 ..< slices {
                try await ZetaApartTest.clear(gpu, Part(texture: t, level: level, slice: slice), depth: 0.9, stencil: 2)
            }
        }
        return t
    }

    static func depth(_ part: Part, _ value: Double) -> Check {
        Check(aspect: .depth, part: part) { _, _ in ZetaApartTest.depth(value) }
    }

    static func stencil(_ part: Part, _ value: UInt32) -> Check {
        Check(aspect: .stencil, part: part) { _, _ in value }
    }

    static func intoLevel(_ gpu: GPU) async throws -> [Check] {
        let t = try await into(gpu, .depth32Float, side: 64, levels: 2)
        try await resolve(gpu, .depth32Float, into: Part(texture: t, level: 1))
        return [depth(Part(texture: t, level: 1), 0.5), depth(Part(texture: t), 0.9)]
    }

    static func intoSlice(_ gpu: GPU) async throws -> [Check] {
        let t = try await into(gpu, .depth32Float, slices: 2)
        try await resolve(gpu, .depth32Float, into: Part(texture: t, slice: 1))
        return [depth(Part(texture: t, slice: 1), 0.5), depth(Part(texture: t), 0.9)]
    }

    static func bothIntoSlice(_ gpu: GPU) async throws -> [Check] {
        let t = try await into(gpu, .depth32Float_stencil8, slices: 2)
        let slice1 = Part(texture: t, slice: 1)
        try await resolve(gpu, .depth32Float_stencil8, stencil: true, into: slice1, filter: .sample0)
        try await resolve(gpu, .depth32Float_stencil8, into: slice1, filter: .min)
        return [depth(slice1, 0.2), stencil(slice1, 7), depth(Part(texture: t), 0.9), stencil(Part(texture: t), 2)]
    }

    static func otherFormats(_ gpu: GPU) async throws -> [Check] {
        let d = Part(texture: try await into(gpu, .depth32Float))
        try await resolve(gpu, .depth32Float_stencil8, into: d)
        let ds = Part(texture: try await into(gpu, .depth32Float_stencil8))
        try await resolve(gpu, .depth32Float, into: ds)
        let s = Part(texture: try await into(gpu, .stencil8))
        try await resolve(gpu, .depth32Float_stencil8, stencil: true, into: s, filter: .sample0)
        return [depth(d, 0.5), depth(ds, 0.5), stencil(ds, 2), stencil(s, 7)]
    }

    static func memoryless(_ gpu: GPU) throws -> [Int] {
        try [(MTLPixelFormat.depth32Float, false), (.stencil8, true)].map { format, stencil in
            let ms = try multisampled(gpu, format)
            let target = try gpu.texture(format, side, side, usage: [.renderTarget], storage: .memoryless)
            let state = try state(gpu, format)
            let written = try ZetaApartTest.writes(gpu)
            return try TextureBufferTargetTest.errorCode(gpu) { commands in
                encodeResolve(
                    commands, from: ms, stencil: stencil, into: Part(texture: target),
                    filter: stencil ? .sample0 : .max, state: state, written: written
                )
            }
        }
    }
}
