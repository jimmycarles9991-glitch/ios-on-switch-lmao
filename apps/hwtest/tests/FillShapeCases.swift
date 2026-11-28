import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The fillTexture: (SPI) shapes gpu.gm20b.fill_shapes fills, each into a texture of its own, as an M4 Max's Metal fills
/// them: every byte of the region at the expected texel and every byte outside it as it was.
public struct FillShapeResult {
    public var path = "-"
    public var insideWrong = 0
    public var outsideChanged = 0

    public init(path: String = "-", insideWrong: Int = 0, outsideChanged: Int = 0) {
        self.path = path
        self.insideWrong = insideWrong
        self.outsideChanged = outsideChanged
    }
}

public enum FillShapeCases {
    public typealias Fill = @convention(c) (AnyObject, Selector, MTLTexture, Int, Int, MTLRegion, MTLClearColor) -> Void
    public typealias FillAs =
        @convention(c) (
            AnyObject, Selector, MTLTexture, Int, Int, MTLRegion, MTLClearColor, MTLPixelFormat
        ) -> Void
    public static let fill = NSSelectorFromString("fillTexture:level:slice:region:color:")
    public static let fillAs = NSSelectorFromString("fillTexture:level:slice:region:color:pixelFormat:")

    /// Each case's key and how it runs; nil when the blit encoder answers neither fill
    public static let all: [(key: String, run: (GPU) async throws -> FillShapeResult?)] = [
        ("rgb9e5", rgb9e5), ("volume", volume), ("samples4", samples4), ("depth_keeps_stencil", depthKeepsStencil),
        ("through_r8", throughR8), ("through_rgba16f", throughHalf), ("cube_slice7", cubeSlice7),
    ]

    public static let region = MTLRegionMake2D(2, 3, 9, 7)

    /// Encodes the fill (`as`: through that format) in a command buffer of its own; the buffer's path, nil without the SPI
    public static func filled(
        _ gpu: GPU, _ texture: MTLTexture, slice: Int = 0, region: MTLRegion = region, _ colour: MTLClearColor,
        as format: MTLPixelFormat? = nil
    ) async throws -> String? {
        var answered = true
        let path = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            let object = blit as AnyObject
            if let format, object.responds(to: fillAs) {
                unsafeBitCast(object.method(for: fillAs), to: FillAs.self)(
                    object, fillAs, texture, 0, slice, region, colour, format
                )
            } else if format == nil, object.responds(to: fill) {
                unsafeBitCast(object.method(for: fill), to: Fill.self)(object, fill, texture, 0, slice, region, colour)
            } else {
                answered = false
            }
            blit.endEncoding()
        }.path
        return answered ? path : nil
    }

    public static func pattern(_ count: Int, seed: Int) -> [UInt8] {
        (0 ..< count).map { UInt8(truncatingIfNeeded: $0 &* 37 &+ seed) }
    }

    /// A 2D texture's bytes after a fill of `region` against `texel` inside (a nil byte: one Metal leaves undefined) and
    /// the uploaded pattern outside
    public static func judged(
        _ got: [UInt8], before: [UInt8], width: Int, texel: [UInt8?], region: MTLRegion = region, path: String
    ) -> FillShapeResult {
        var result = FillShapeResult(path: path)
        let size = texel.count
        for at in 0 ..< got.count / size {
            let (x, y) = (at % width, at / width)
            let inside =
                x >= region.origin.x && x < region.origin.x + region.size.width && y >= region.origin.y
                    && y < region.origin.y + region.size.height
            let bytes = Array(got[at * size ..< (at + 1) * size])
            if inside { result.insideWrong += zip(bytes, texel).filter { $1 != nil && $0 != $1 }.count }
            if !inside {
                result.outsideChanged += zip(bytes, before[at * size ..< (at + 1) * size]).filter { $0 != $1 }.count
            }
        }
        return result
    }

    /// RGB9E5 at (0.25, 0.5, 0.75, 1): 0x7E020080
    static func rgb9e5(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 16
        let texture = try gpu.texture(.rgb9e5Float, side, side, usage: [.shaderRead])
        let before = pattern(side * side * 4, seed: 1)
        try await gpu.upload(before, to: texture, bytesPerRow: side * 4)
        guard let path = try await filled(gpu, texture, MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1))
        else { return nil }
        let got = try await gpu.read(texture, bytesPerPixel: 4)
        return judged(got, before: before, width: side, texel: [0x80, 0x00, 0x02, 0x7e], path: path)
    }

    /// An RGBA8 3D texture's planes 1 to 3 of a region
    static func volume(_ gpu: GPU) async throws -> FillShapeResult? {
        let (side, depth) = (8, 5)
        let d = MTLTextureDescriptor()
        d.textureType = .type3D
        d.pixelFormat = .rgba8Unorm
        (d.width, d.height, d.depth) = (side, side, depth)
        d.usage = .shaderRead
        d.storageMode = .private
        guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no 3D texture") }
        let plane = side * side * 4
        let before = pattern(plane * depth, seed: 3)
        let staging = try gpu.buffer(before)
        try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(
                from: staging, sourceOffset: 0, sourceBytesPerRow: side * 4, sourceBytesPerImage: plane,
                sourceSize: MTLSize(width: side, height: side, depth: depth), to: texture, destinationSlice: 0,
                destinationLevel: 0, destinationOrigin: MTLOrigin()
            )
            blit.endEncoding()
        }
        let region = MTLRegionMake3D(1, 2, 1, 5, 4, 3)
        let colour = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.8)
        guard let path = try await filled(gpu, texture, region: region, colour) else { return nil }
        var result = FillShapeResult(path: path)
        for z in 0 ..< depth {
            let got = try await gpu.read(texture, bytesPerPixel: 4, plane: z)
            let inside = z >= 1 && z < 4 ? region : MTLRegionMake2D(0, 0, 0, 0)
            let one = judged(
                got, before: Array(before[z * plane ..< (z + 1) * plane]), width: side, texel: [51, 102, 153, 204],
                region: inside, path: path
            )
            result.insideWrong += one.insideWrong
            result.outsideChanged += one.outsideChanged
        }
        return result
    }

    /// Every sample of a 4x texture cleared blue, read through a resolve: red in the region
    static func samples4(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 16
        let texture = try gpu.texture(.rgba8Unorm, side, side, usage: [.renderTarget], samples: 4)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        guard let path = try await filled(gpu, texture, MTLClearColor(red: 1, green: 0, blue: 0, alpha: 1)) else {
            return nil
        }
        let one = try gpu.texture(.rgba8Unorm, side, side)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .load
            pass.colorAttachments[0].storeAction = .multisampleResolve
            pass.colorAttachments[0].resolveTexture = one
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        let got = try await gpu.read(one, bytesPerPixel: 4)
        let blue = Array([[UInt8]](repeating: [0, 0, 255, 255], count: side * side).joined())
        return judged(got, before: blue, width: side, texel: [255, 0, 0, 255], path: path)
    }

    /// Depth32Float_Stencil8 cleared to depth 0.5 and stencil 0x5a, filled through R32Float with red 0.25: the region's
    /// depth 0.25, every stencil kept. A fill without a format writes no defined depth on an M4 Max
    static func depthKeepsStencil(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 16
        let texture = try gpu.texture(.depth32Float_stencil8, side, side, usage: [.renderTarget])
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.depthAttachment.texture = texture
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.clearDepth = 0.5
            pass.depthAttachment.storeAction = .store
            pass.stencilAttachment.texture = texture
            pass.stencilAttachment.loadAction = .clear
            pass.stencilAttachment.clearStencil = 0x5a
            pass.stencilAttachment.storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        guard
            let path = try await filled(
                gpu, texture, MTLClearColor(red: 0.25, green: 0.75, blue: 0.125, alpha: 0.375), as: .r32Float
            )
        else { return nil }
        let depth = try await gpu.read(texture, bytesPerPixel: 4, options: .depthFromDepthStencil)
        let stencil = try await gpu.read(texture, bytesPerPixel: 1, options: .stencilFromDepthStencil)
        let half = Array([[UInt8]](repeating: le(Float(0.5).bitPattern), count: side * side).joined())
        var result = judged(depth, before: half, width: side, texel: le(Float(0.25).bitPattern), path: path)
        result.outsideChanged += stencil.filter { $0 != 0x5a }.count
        return result
    }

    /// RGBA32Float filled through R8Unorm 0.25: byte 0 of each texel 0x40. An M4 Max leaves the other 15 undefined: zeroes
    /// in a process's first fill, an earlier fill's colour bytes after it, and in bytes 8 to 15 its own memory
    static func throughR8(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 16
        let texture = try gpu.texture(.rgba32Float, side, side, usage: [.shaderRead])
        let before = pattern(side * side * 16, seed: 5)
        try await gpu.upload(before, to: texture, bytesPerRow: side * 16)
        guard
            let path = try await filled(
                gpu, texture, MTLClearColor(red: 0.25, green: 0, blue: 0, alpha: 0), as: .r8Unorm
            )
        else { return nil }
        let got = try await gpu.read(texture, bytesPerPixel: 16)
        let texel: [UInt8?] = [0x40] + [UInt8?](repeating: nil, count: 15)
        return judged(got, before: before, width: side, texel: texel, path: path)
    }

    /// RGBA8 filled through RGBA16Float (0.25, 0.5, 0.75, 1): the half texel cut to 4 bytes, 00 34 00 38
    static func throughHalf(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 16
        let texture = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        let before = pattern(side * side * 4, seed: 7)
        try await gpu.upload(before, to: texture, bytesPerRow: side * 4)
        guard
            let path = try await filled(
                gpu, texture, MTLClearColor(red: 0.25, green: 0.5, blue: 0.75, alpha: 1), as: .rgba16Float
            )
        else { return nil }
        let got = try await gpu.read(texture, bytesPerPixel: 4)
        return judged(got, before: before, width: side, texel: [0x00, 0x34, 0x00, 0x38], path: path)
    }

    /// A cube array's slice 7, the second cube's face 1: that face whole, every other face as it was
    static func cubeSlice7(_ gpu: GPU) async throws -> FillShapeResult? {
        let side = 4
        let d = MTLTextureDescriptor()
        d.textureType = .typeCubeArray
        d.pixelFormat = .rgba8Unorm
        (d.width, d.height, d.arrayLength) = (side, side, 2)
        d.usage = .shaderRead
        d.storageMode = .private
        guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no cube array") }
        let face = side * side * 4
        var before: [[UInt8]] = []
        for slice in 0 ..< 12 {
            before.append(pattern(face, seed: 11 * slice + 1))
            try await gpu.upload(before[slice], to: texture, bytesPerRow: side * 4, slice: slice)
        }
        let whole = MTLRegionMake2D(0, 0, side, side)
        guard
            let path = try await filled(
                gpu, texture, slice: 7, region: whole, MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.8)
            )
        else { return nil }
        var result = FillShapeResult(path: path)
        for slice in 0 ..< 12 {
            let got = try await gpu.read(texture, bytesPerPixel: 4, slice: slice)
            let one = judged(
                got, before: before[slice], width: side, texel: [51, 102, 153, 204],
                region: slice == 7 ? whole : MTLRegionMake2D(0, 0, 0, 0), path: path
            )
            result.insideWrong += one.insideWrong
            result.outsideChanged += one.outsideChanged
        }
        return result
    }

    static func le(_ word: UInt32) -> [UInt8] { (0 ..< 4).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) } }
}
