import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// One image of a texture: a level and a slice (a cube's face, a cube array's face of a cube, a 3D level's plane)
struct TexturePart: Hashable {
    let level: Int, slice: Int
}

/// Parts of RGBA8 textures as render targets: every part of every texture cleared to a colour of its own, then a quad
/// drawn over the left half of one, then every part read back by the blit engine
enum TextureParts {
    /// Each part's clear colour, distinct in every texture of up to five levels and 16 slices, exact in unorm8
    static func clearColor(_ p: TexturePart) -> [UInt8] {
        [UInt8(10 + 40 * p.level), UInt8(20 + 14 * p.slice), 200, 255]
    }

    static let drawn: [UInt8] = [255, 0, 128, 255]

    static func size(_ t: MTLTexture, _ level: Int) -> (width: Int, height: Int, depth: Int) {
        (
            max(1, t.width >> level), max(1, t.height >> level),
            t.textureType == .type3D ? max(1, t.depth >> level) : 1
        )
    }

    /// Every part of `t`: its levels' slices (6 a cube), a 3D level's planes
    static func parts(_ t: MTLTexture) -> [TexturePart] {
        var out: [TexturePart] = []
        for level in 0..<t.mipmapLevelCount {
            let slices: Int
            switch t.textureType {
            case .type3D: slices = size(t, level).depth
            case .typeCube: slices = 6
            case .typeCubeArray: slices = 6 * t.arrayLength
            default: slices = t.arrayLength
            }
            for slice in 0..<slices { out.append(TexturePart(level: level, slice: slice)) }
        }
        return out
    }

    /// A pass into part `p`: its slice, or a 3D texture's depth plane
    static func pass(_ t: MTLTexture, _ p: TexturePart, load: MTLLoadAction) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        let a = pass.colorAttachments[0]!
        a.texture = t
        a.level = p.level
        if t.textureType == .type3D { a.depthPlane = p.slice } else { a.slice = p.slice }
        a.loadAction = load
        let c = clearColor(p).map { Double($0) / 255 }
        a.clearColor = MTLClearColor(red: c[0], green: c[1], blue: c[2], alpha: c[3])
        a.storeAction = .store
        return pass
    }

    /// Part `p`'s texels, rows packed, copied by the blit engine
    static func read(_ gpu: GPU, _ t: MTLTexture, _ p: TexturePart, paths: inout [String]) async throws -> [UInt8] {
        let (w, h, _) = size(t, p.level)
        let volume = t.textureType == .type3D
        let out = try gpu.buffer(w * h * 4)
        let timing = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: t, sourceSlice: volume ? 0 : p.slice, sourceLevel: p.level,
                sourceOrigin: MTLOrigin(x: 0, y: 0, z: volume ? p.slice : 0),
                sourceSize: MTLSize(width: w, height: h, depth: 1), to: out, destinationOffset: 0,
                destinationBytesPerRow: w * 4, destinationBytesPerImage: 0)
            blit.endEncoding()
        }
        paths.append(timing.path)
        return [UInt8](UnsafeRawBufferPointer(start: out.contents(), count: w * h * 4))
    }

    /// Clears every part of `t`, draws into the left half of `target`, and counts the texels of each part that are not
    /// what that leaves: the drawn colour in the target's left half, every other texel its part's clear colour
    static func check(
        _ gpu: GPU, _ t: MTLTexture, target: TexturePart, pipeline: MTLRenderPipelineState, paths: inout [String]
    ) async throws -> Int {
        let parts = parts(t)
        let (tw, th, _) = size(t, target.level)
        let timing = try await gpu.run { commands in
            for p in parts { commands.makeRenderCommandEncoder(descriptor: pass(t, p, load: .clear))!.endEncoding() }
            let e = commands.makeRenderCommandEncoder(descriptor: pass(t, target, load: .load))!
            e.setRenderPipelineState(pipeline)
            GPU.draw(
                e, corners: GPU.pixelQuad(0, 0, tw / 2, th, width: tw, height: th),
                color: SIMD4<Float>(drawn.map { Float($0) / 255 }))
            e.endEncoding()
        }
        paths.append(timing.path)
        var wrong = 0
        for p in parts {
            let (w, h, _) = size(t, p.level)
            let got = try await read(gpu, t, p, paths: &paths)
            for y in 0..<h {
                for x in 0..<w {
                    let want = p == target && x < tw / 2 ? drawn : clearColor(p)
                    if Array(got.pixel(x, y, width: w)) != want { wrong += 1 }
                }
            }
        }
        return wrong
    }
}

/// Render targets of a mip level, an array slice, a cube face, a cube array's face and a 3D plane, in textures of one
/// level and mipmapped ones: each binds the part NVK binds (raw_attachment_target.h), so the quad lands in that part and
/// every other texel of the texture keeps its clear.
struct NativeTargetPartsTest: NativeObjectTest {
    let id = "metal.native.target_parts"
    let name = "Render targets of levels, slices, cube faces and 3D planes, each texel of each texture checked"

    struct Shape {
        let name: String
        let descriptor: MTLTextureDescriptor
        let target: TexturePart
    }

    static func descriptor(
        _ type: MTLTextureType, _ width: Int, _ height: Int, depth: Int = 1, layers: Int = 1, levels: Int = 1
    ) -> MTLTextureDescriptor {
        let d = MTLTextureDescriptor()
        d.textureType = type
        d.pixelFormat = .rgba8Unorm
        d.width = width
        d.height = height
        d.depth = depth
        d.arrayLength = layers
        d.mipmapLevelCount = levels
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .private
        return d
    }

    static let shapes: [Shape] = [
        Shape(name: "mip", descriptor: descriptor(.type2D, 64, 48, levels: 4), target: TexturePart(level: 2, slice: 0)),
        Shape(
            name: "array", descriptor: descriptor(.type2DArray, 32, 24, layers: 3),
            target: TexturePart(level: 0, slice: 2)),
        Shape(
            name: "mip_array", descriptor: descriptor(.type2DArray, 32, 32, layers: 3, levels: 3),
            target: TexturePart(level: 1, slice: 1)),
        Shape(name: "cube", descriptor: descriptor(.typeCube, 32, 32), target: TexturePart(level: 0, slice: 3)),
        Shape(
            name: "mip_cube", descriptor: descriptor(.typeCube, 32, 32, levels: 3),
            target: TexturePart(level: 1, slice: 5)),
        Shape(
            name: "cube_array", descriptor: descriptor(.typeCubeArray, 16, 16, layers: 2),
            target: TexturePart(level: 0, slice: 8)),
        Shape(
            name: "volume", descriptor: descriptor(.type3D, 16, 16, depth: 8), target: TexturePart(level: 0, slice: 5)),
        Shape(
            name: "mip_volume", descriptor: descriptor(.type3D, 16, 16, depth: 8, levels: 2),
            target: TexturePart(level: 1, slice: 2)),
    ]

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        for shape in Self.shapes {
            guard let t = gpu.device.makeTexture(descriptor: shape.descriptor) else {
                work.failures.append("no \(shape.name) texture")
                continue
            }
            work.objects.append(t)
            let wrong = try await TextureParts.check(
                gpu, t, target: shape.target, pipeline: pipeline, paths: &work.paths)
            metrics.set("wrong_\(shape.name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) texels of the \(shape.name) texture") }
        }
        return work
    }
}
