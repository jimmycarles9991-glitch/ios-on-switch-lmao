import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Depth/stencil textures past stored render targets that gpu.gm20b.texture_shapes_depth does not reach, each raw's own
/// zeta memory (ShapeArms): a private Depth32Float only sampled, filled and read back by blits; shared Depth16Unorm
/// (13x7), a shared Depth32Float chain's level 1 and a shared Stencil8 cube's face 3 through replaceRegion and getBytes;
/// a shared 4x Depth32Float cleared to 0.25 read by getBytes in every sample; a private Depth32Float_Stencil8 chain
/// cleared to 0.25 / 0x5a read back by depth and stencil blits (Metal's validation stops its CPU copies); a pass into
/// level 1 of a Depth32Float chain through a view, and directly into level 2 / slice 1 of a 100x60 depth array,
/// preserving the other levels and slices, each read back by blits; a depth cube array
/// (the A13's alone: made on iOS 27, nil on iOS 17) and a private 4x Depth16Unorm only sampled.
struct DepthShapesTest: HWTest {
    let id = "gpu.gm20b.depth_shapes"
    let name = "Depth/stencil textures of every usage, storage, level and layer are raw's own"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    private static let quarter: [UInt8] = [0, 0, 0x80, 0x3e]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        if let skip = ShapeArms.skip(gpu) { return skip }
        let arms = ShapeArms(gpu)

        let floats = (0 ..< 64).flatMap { withUnsafeBytes(of: Float($0) / 64) { Array($0) } }
        let read = arms.make(.depth32Float, .type2D, 8, 8, usage: .shaderRead)
        arms.route("read", read, expect: "gm20b")
        if let read {
            try await gpu.upload(floats, to: read, bytesPerRow: 32)
            arms.off("read", try await gpu.read(read, bytesPerPixel: 4), floats)
        }
        try cpuArms(arms)
        try await samplesArm(arms)
        try await stencilBlitArm(arms)
        try await levelArms(arms)

        let a13 = ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27
        arms.route(
            "cube_array", arms.make(.depth32Float, .typeCubeArray, 8, 8, array: 2, usage: .shaderRead),
            expect: a13 ? "gm20b" : "nil"
        )
        arms.route(
            "ms_read", arms.make(.depth16Unorm, .type2DMultisample, 8, 8, samples: 4, usage: .shaderRead),
            expect: "gm20b"
        )
        return arms.outcome()
    }

    private func cpuArms(_ arms: ShapeArms) throws {
        let shared = arms.make(.depth16Unorm, .type2D, 13, 7, storage: .shared, usage: .shaderRead)
        arms.route("shared", shared, expect: "gm20b")
        if let shared {
            arms.off("shared", try arms.cpuRoundTrip(shared, width: 13, height: 7, bytesPerRow: 26, rows: 7), [])
        }
        let chain = arms.make(.depth32Float, .type2D, 16, 16, levels: 3, storage: .shared, usage: .shaderRead)
        arms.route("chain", chain, expect: "gm20b")
        if let chain {
            arms.off("chain", try arms.cpuRoundTrip(chain, width: 8, height: 8, bytesPerRow: 32, rows: 8, level: 1), [])
        }
        let cube = arms.make(.stencil8, .typeCube, 8, 8, storage: .shared, usage: .shaderRead)
        arms.route("stencil_cube", cube, expect: "gm20b")
        if let cube {
            arms.off(
                "stencil_cube", try arms.cpuRoundTrip(cube, width: 8, height: 8, bytesPerRow: 8, rows: 8, slice: 3), []
            )
        }
    }

    private func samplesArm(_ arms: ShapeArms) async throws {
        let samples = try arms.gpu.texture(.depth32Float, 4, 4, samples: 4, storage: .shared)
        arms.route("ms", samples, expect: "gm20b")
        try await clear(arms.gpu, samples, stencil: false)
        var cleared = [UInt8](repeating: 0, count: 4 * 4 * 4 * 4)
        samples.getBytes(&cleared, bytesPerRow: 64, from: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0)
        arms.off("ms", cleared, Array([[UInt8]](repeating: Self.quarter, count: 64).joined()))
    }

    private func stencilBlitArm(_ arms: ShapeArms) async throws {
        let both = arms.make(.depth32Float_stencil8, .type2D, 32, 32, levels: 3, usage: [.renderTarget, .shaderRead])
        arms.route("d32s8", both, expect: "gm20b")
        guard let both else { return }
        try await clear(arms.gpu, both, stencil: true)
        let depth = try await arms.gpu.read(both, bytesPerPixel: 4, options: .depthFromDepthStencil)
        arms.off("d32s8_depth", depth, Array([[UInt8]](repeating: Self.quarter, count: 32 * 32).joined()))
        let stencil = try await arms.gpu.read(both, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        arms.off("d32s8_stencil", stencil, [UInt8](repeating: 0x5a, count: 32 * 32))
    }

    private func levelArms(_ arms: ShapeArms) async throws {
        let levels = arms.make(.depth32Float, .type2D, 16, 16, levels: 3, usage: [.renderTarget, .shaderRead])
        let view = levels?.makeTextureView(
            pixelFormat: .depth32Float, textureType: .type2D, levels: 1 ..< 2, slices: 0 ..< 1
        )
        arms.route("level_view", view, expect: "gm20b")
        if let levels, let view {
            try await clear(arms.gpu, view, stencil: false)
            let level = try await arms.gpu.read(levels, bytesPerPixel: 4, level: 1)
            arms.off("level_view", level, Array([[UInt8]](repeating: Self.quarter, count: 8 * 8).joined()))
        }
        let direct = arms.make(
            .depth32Float, .type2DArray, 100, 60, levels: 4, array: 3, usage: [.renderTarget, .shaderRead]
        )
        arms.route("level_direct", direct, expect: "gm20b")
        guard let direct else { return }
        for level in 0 ..< 4 {
            for slice in 0 ..< 3 {
                let word = withUnsafeBytes(of: Float(16 + 3 * level + slice) / 32) { Array($0) }
                let bytes = Array([[UInt8]](repeating: word, count: (100 >> level) * (60 >> level)).joined())
                try await arms.gpu.upload(
                    bytes, to: direct, bytesPerRow: (100 >> level) * 4, level: level, slice: slice
                )
            }
        }
        let timing = try await arms.gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = direct
            pass.depthAttachment.level = 2
            pass.depthAttachment.slice = 1
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth = 0.25
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        arms.set("level_direct_completed", 1)
        arms.set("level_direct_raw", timing.path == "gm20b" ? 1 : 0)
        var neighbors: [UInt8] = [], expected: [UInt8] = []
        for level in 0 ..< 4 {
            for slice in 0 ..< 3 {
                let got = try await arms.gpu.read(direct, bytesPerPixel: 4, level: level, slice: slice)
                let count = (100 >> level) * (60 >> level)
                if level == 2 && slice == 1 {
                    arms.off("level_direct", got, Array([[UInt8]](repeating: Self.quarter, count: count).joined()))
                } else {
                    let word = withUnsafeBytes(of: Float(16 + 3 * level + slice) / 32) { Array($0) }
                    neighbors += got
                    expected += Array([[UInt8]](repeating: word, count: count).joined())
                }
            }
        }
        arms.off("level_direct_neighbors", neighbors, expected)
    }

    /// A pass that clears `texture`'s depth to 0.25 (and its stencil to 0x5a) and stores it
    private func clear(_ gpu: GPU, _ texture: MTLTexture, stencil: Bool) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = texture
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .store
            pass.depthAttachment.clearDepth = 0.25
            if stencil {
                pass.stencilAttachment.texture = texture
                pass.stencilAttachment.loadAction = .clear
                pass.stencilAttachment.storeAction = .store
                pass.stencilAttachment.clearStencil = 0x5a
            }
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
    }
}
