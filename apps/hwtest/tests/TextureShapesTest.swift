import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// newTextureWithDescriptor: shapes that went to m2v, one a group (ShapeArms): each made texture is raw's own (after
/// its work too), its command buffers are raw's, and it reads back what was put in it; each shape the identity, GM20B
/// or Metal makes none of answers nil. Cube arrays are the A13's alone: made on iOS 27, nil on iOS 17. PVRTC is made
/// on both and ASTC HDR on iOS 27 alone (the A10X makes none): GM20B decodes neither, so their blocks are decoded on
/// the GPU as they are uploaded (decoded.h). A BC1 3D texture's planes and levels read back as their solid blocks;
/// a signed normalized target keeps Metal's clear codes.
struct TextureShapesTest: HWTest {
    let id = "gpu.gm20b.texture_shapes"
    let name = "Texture shapes are raw's own, or nil where the identity makes none"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        if let skip = ShapeArms.skip(gpu) { return skip }
        let arms = ShapeArms(gpu)

        let snorm = try gpu.texture(.rgba8Snorm, 4, 4, usage: [.shaderRead])
        arms.route("snorm", snorm, expect: "gm20b")
        let snormTexels = Array([[UInt8]](repeating: [127, 0, 127, 129], count: 16).joined())
        try await gpu.upload(snormTexels, to: snorm, bytesPerRow: 16)
        arms.off(
            "snorm", try await arms.sampled(snorm), Array([[UInt8]](repeating: [255, 0, 255, 0], count: 16).joined())
        )

        let snormTarget = try gpu.texture(.rgba8Snorm, 4, 4, usage: [.renderTarget, .shaderRead])
        arms.route("snorm_target", snormTarget, expect: "gm20b")
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = snormTarget
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(red: -1, green: -0.5, blue: 0.5, alpha: 1)
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        arms.off(
            "snorm_target", try await gpu.read(snormTarget, bytesPerPixel: 4),
            Array([[UInt8]](repeating: [129, 192, 64, 127], count: 16).joined())
        )

        let chain = try gpu.texture(.r8Unorm_srgb, 4, 4, usage: [.shaderRead], storage: .shared, mipmaps: 3)
        arms.route("srgb_chain", chain, expect: "gm20b")
        arms.off("srgb_chain", try arms.cpuRoundTrip(chain, width: 2, height: 2, bytesPerRow: 2, rows: 2, level: 1), [])

        let volume = arms.make(.rgba8Unorm, .type3D, 8, 8, depth: 4, storage: .memoryless, usage: .renderTarget)
        arms.route("memoryless_3d", volume, expect: "gm20b")

        let pairs = arms.make(.gbgr422, .type2D, 8, 2, storage: .shared, usage: .shaderRead)
        arms.route("gbgr422", pairs, expect: "gm20b")
        if let pairs {
            arms.off("gbgr422", try arms.cpuRoundTrip(pairs, width: 8, height: 2, bytesPerRow: 16, rows: 2), [])
        }

        let cube = arms.make(.bc1_rgba, .typeCube, 8, 8, storage: .shared, usage: .shaderRead)
        arms.route("bc1_cube", cube, expect: "gm20b")
        if let cube {
            arms.off(
                "bc1_cube", try arms.cpuRoundTrip(cube, width: 8, height: 8, bytesPerRow: 16, rows: 2, slice: 3), []
            )
        }

        let samples = try gpu.texture(.rgba8Unorm, 4, 4, samples: 4, storage: .shared)
        arms.route("shared_ms", samples, expect: "gm20b")
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = samples
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1)
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        var cleared = [UInt8](repeating: 0, count: 4 * 4 * 4 * 4)
        samples.getBytes(&cleared, bytesPerRow: 64, from: MTLRegionMake2D(0, 0, 4, 4), mipmapLevel: 0)
        arms.off("shared_ms", cleared, Array([[UInt8]](repeating: [64, 128, 191, 255], count: 64).joined()))

        let a13 = ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27
        arms.route(
            "cube_array", arms.make(.rgba8Unorm, .typeCubeArray, 8, 8, array: 2, usage: .shaderRead),
            expect: a13 ? "gm20b" : "nil"
        )
        arms.route(
            "ms_array", arms.make(.rgba8Unorm, .type2DMultisampleArray, 8, 8, samples: 4, usage: .renderTarget),
            expect: "nil"
        )
        arms.route(
            "pvrtc", arms.make(MTLPixelFormat(rawValue: 166)!, .type2D, 8, 8, usage: .shaderRead), expect: "gm20b"
        )
        arms.route(
            "astc_hdr", arms.make(MTLPixelFormat(rawValue: 222)!, .type2D, 8, 8, usage: .shaderRead),
            expect: a13 ? "gm20b" : "nil"
        )
        try await blockVolumes(arms)
        arms.route(
            "line_levels", arms.make(.rgba8Unorm, .type1D, 16, 1, levels: 3, usage: .shaderRead), expect: "nil"
        )
        return arms.outcome()
    }

    /// A shared BC1 3D texture: a plane's blocks through replaceRegion and getBytes, and planes 0 and 1 of level 0 and
    /// plane 0 of level 1, solid red, green and blue blocks, read by view_read_volume (this Mac's Metal reads the same:
    /// m2vf-shapes2 probe/hwref/bc3d.swift); a private ASTC 4x4 3D texture made
    private func blockVolumes(_ arms: ShapeArms) async throws {
        let blockVolume = arms.make(.bc1_rgba, .type3D, 8, 8, depth: 4, levels: 2, storage: .shared, usage: .shaderRead)
        arms.route("bc1_3d", blockVolume, expect: "gm20b")
        if let blockVolume {
            arms.off(
                "bc1_3d",
                try arms.cpuRoundTrip(blockVolume, width: 8, height: 8, bytesPerRow: 16, rows: 2, plane: 2), []
            )
            for (level, plane, colour) in [(0, 0, 0xf800), (0, 1, 0x07e0), (1, 0, 0x001f)] {
                let lo = UInt8(colour & 0xff), hi = UInt8(colour >> 8)
                let block: [UInt8] = [lo, hi, lo, hi, 0, 0, 0, 0]
                let side = level == 0 ? 8 : 4
                let solid = Array([[UInt8]](repeating: block, count: (side / 4) * (side / 4)).joined())
                blockVolume.replace(
                    region: MTLRegionMake3D(0, 0, plane, side, side, 1), mipmapLevel: level, slice: 0,
                    withBytes: solid, bytesPerRow: 8 * (side / 4), bytesPerImage: solid.count
                )
            }
            arms.off(
                "bc1_3d_read", try await arms.drawn(blockVolume, fragment: "view_read_volume", width: 3),
                [255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255]
            )
        }
        arms.route("astc_3d", arms.make(.astc_4x4_ldr, .type3D, 8, 8, depth: 4, usage: .shaderRead), expect: "gm20b")
    }
}
