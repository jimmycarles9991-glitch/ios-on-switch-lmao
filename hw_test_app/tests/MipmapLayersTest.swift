import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// generateMipmaps of arrays and cubes whose steps are the 3D engine's (formats with a pack mip state and no 2D-engine
/// filter: drivers/TegraGM20BMetal/raw/runtime/raw_mip_passes.cpp, one pass a layer and level), which went to m2v
/// (mipmaps) before. Each layer's level 0 is cleared to its own colour, so every level of it must read that colour back.
/// The generateMipmaps command buffer must stay raw; the clears' path is reported.
struct MipmapLayersTest: HWTest {
    let id = "gpu.raw.mipmap_layers"
    let name = "Mipmaps of arrays, cubes and texture views"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        let cases: [(String, MTLPixelFormat, MTLTextureType, Int)] = [
            ("srgb_array", .rgba8Unorm_srgb, .type2DArray, 3), ("srgb_cube", .rgba8Unorm_srgb, .typeCube, 6),
            ("rgb10a2_array", .rgb10a2Unorm, .type2DArray, 3),
        ]
        for (label, format, type, layers) in cases {
            let d = MTLTextureDescriptor()
            d.pixelFormat = format
            d.textureType = type
            d.width = 16
            d.height = 16
            d.arrayLength = type == .typeCube ? 1 : layers
            d.mipmapLevelCount = 5
            d.usage = [.renderTarget, .shaderRead]
            d.storageMode = .private
            guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(label)") }
            let fill = try await gpu.run { commands in
                for layer in 0..<layers {
                    let p = MTLRenderPassDescriptor()
                    p.colorAttachments[0].texture = texture
                    p.colorAttachments[0].slice = layer
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].clearColor = MTLClearColor(
                        red: Double(layer + 1) / 8, green: 0.25, blue: 1 - Double(layer) / 8, alpha: 1)
                    p.colorAttachments[0].storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
                }
            }.path
            let generated = try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.generateMipmaps(for: texture)
                blit.endEncoding()
            }.path
            var off = 0
            var firsts: [String] = []
            for layer in 0..<layers {
                let top = try await gpu.read(texture, bytesPerPixel: 4, level: 0, slice: layer)
                let first = Array(top[0..<4])
                firsts.append(first.map { String($0) }.joined(separator: "/"))
                for level in 1..<5 {
                    let bytes = try await gpu.read(texture, bytesPerPixel: 4, level: level, slice: layer)
                    let side = 16 >> level
                    for p in 0..<(side * side) where Array(bytes[4 * p..<4 * p + 4]) != first { off += 1 }
                }
            }
            metrics.set("\(label)_fill_path", fill)
            metrics.set("\(label)_path", generated)
            metrics.set("\(label)_wrong", off)
            metrics.set("\(label)_texels", firsts.joined(separator: ","))
            if off > 0 { wrong.append(label) }
            if generated != "raw" { wrong.append("\(label)_path") }
        }
        let cubeArrays = gpu.device.supportsFamily(.apple4)
        for arm in [
            (name: "nested_array_view", cube: false, srgb: false, nested: true),
            (name: "srgb_array_view", cube: false, srgb: true, nested: false),
            (name: "cube_view", cube: true, srgb: false, nested: false),
        ] {
            let layers = arm.cube ? (cubeArrays ? 12 : 6) : 3
            let firstLevel = arm.nested ? 2 : 1
            let firstSlice = arm.cube ? (cubeArrays ? 6 : 0) : 1
            let sliceCount = arm.cube ? 6 : 1
            let d = MTLTextureDescriptor()
            d.textureType = arm.cube ? (cubeArrays ? .typeCubeArray : .typeCube) : .type2DArray
            d.pixelFormat = .rgba8Unorm
            d.width = 32
            d.height = 32
            d.arrayLength = arm.cube ? (cubeArrays ? 2 : 1) : layers
            d.mipmapLevelCount = 6
            d.usage = [.shaderRead, .pixelFormatView]
            d.storageMode = .shared
            guard let texture = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no \(arm.name)") }
            var initial: [[UInt8]] = []
            for layer in 0..<layers {
                for level in 0..<6 {
                    let side = 32 >> level
                    var bytes = [UInt8](repeating: UInt8(160 + layer * 4), count: side * side * 4)
                    for p in 0..<(side * side) {
                        let color =
                            level == firstLevel && (firstSlice..<firstSlice + sliceCount).contains(layer)
                            ? UInt8(((p % side + p / side) & 1) * 255) : UInt8(16 + layer * 8 + level)
                        bytes[4 * p] = color
                        bytes[4 * p + 1] = color
                        bytes[4 * p + 2] = color
                    }
                    texture.replace(
                        region: MTLRegionMake2D(0, 0, side, side), mipmapLevel: level, slice: layer,
                        withBytes: bytes, bytesPerRow: side * 4, bytesPerImage: side * side * 4)
                    initial.append(bytes)
                }
            }
            let format: MTLPixelFormat = arm.srgb ? .rgba8Unorm_srgb : .rgba8Unorm
            let type: MTLTextureType = arm.cube ? .typeCube : .type2DArray
            guard
                let outer = texture.makeTextureView(
                    pixelFormat: format, textureType: type, levels: 1..<(arm.nested ? 5 : 4),
                    slices: firstSlice..<(firstSlice + (arm.nested ? 2 : sliceCount)))
            else { throw TestFailure("no \(arm.name) view") }
            var view = outer
            if arm.nested {
                guard
                    let inner = outer.makeTextureView(
                        pixelFormat: format, textureType: type, levels: 1..<3, slices: 0..<1)
                else { throw TestFailure("no nested mip view") }
                view = inner
            }
            let generated = try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.generateMipmaps(for: view)
                blit.endEncoding()
            }.path
            var off = 0
            for layer in 0..<layers {
                for level in 0..<6 {
                    var expected = initial[layer * 6 + level]
                    if (firstSlice..<firstSlice + sliceCount).contains(layer) && (firstLevel + 1..<4).contains(level) {
                        let color: UInt8 = arm.srgb ? 188 : 128
                        for p in 0..<(expected.count / 4) {
                            expected[4 * p] = color
                            expected[4 * p + 1] = color
                            expected[4 * p + 2] = color
                        }
                    }
                    let bytes = try await gpu.read(texture, bytesPerPixel: 4, level: level, slice: layer)
                    for i in expected.indices where bytes[i] != expected[i] { off += 1 }
                }
            }
            metrics.set("\(arm.name)_path", generated)
            metrics.set("\(arm.name)_wrong", off)
            if off > 0 { wrong.append(arm.name) }
            if generated != "raw" { wrong.append("\(arm.name)_path") }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }
}
