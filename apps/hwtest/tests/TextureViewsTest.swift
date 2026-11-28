import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Texture views of a part of a texture on the driver (drivers/TegraGM20BMetal/texture_views.cpp
/// createPartView and createZetaView). Each level, slice, face and plane is cleared to its own colour by a pass into
/// it, then a draw reads it through the view: level 1 of slice 1 of a mipmapped array as a 2D texture, the last six
/// slices of an array as a cube, levels 1 and 2 of a mipmapped 3D texture, slice 2 and slices 1-2 of a depth array, and
/// a depth/stencil texture's stencil (X32_Stencil8), a buffer's texture and a multisampled one as one-layer arrays.
/// Every command buffer that reads a view must stay raw; the fills' path is reported (passes into levels, slices and
/// depth layers are pass.level's and pass.ds's), and so are the cube view's texture header and its array's (cube_tic,
/// cubes_tic: the eight words switchMetalProbeHeader: answers).
struct TextureViewsTest: HWTest {
    let id = "gpu.gm20b.texture_views"
    let name = "Views of levels, slices, faces, 3D levels, depth layers and stencil read their part, on the driver"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static func code(_ level: Int, _ slice: Int) -> [UInt8] { [UInt8(16 * slice + level + 1), 40, 200, 255] }

    static func clear(_ code: [UInt8]) -> MTLClearColor {
        MTLClearColor(
            red: Double(code[0]) / 255, green: Double(code[1]) / 255, blue: Double(code[2]) / 255,
            alpha: Double(code[3]) / 255
        )
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        var paths: [String] = []
        func colour(
            _ format: MTLPixelFormat, _ type: MTLTextureType, _ w: Int, _ h: Int, depth: Int = 1,
            slices: Int = 1, levels: Int = 1
        ) throws -> MTLTexture {
            let d = MTLTextureDescriptor()
            d.pixelFormat = format
            d.textureType = type
            d.width = w
            d.height = h
            d.depth = depth
            d.arrayLength = slices
            d.mipmapLevelCount = levels
            d.usage = [.renderTarget, .shaderRead, .pixelFormatView]
            d.storageMode = .private
            guard let t = gpu.device.makeTexture(descriptor: d) else {
                throw TestFailure("no \(type.rawValue) texture")
            }
            return t
        }
        let array = try colour(.rgba8Unorm, .type2DArray, 16, 16, slices: 2, levels: 5)
        let cubes = try colour(.rgba8Unorm, .type2DArray, 8, 8, slices: 12)
        let volume = try colour(.rgba8Unorm, .type3D, 8, 8, depth: 4, levels: 3)
        let depths = try colour(.depth32Float, .type2DArray, 8, 8, slices: 3)
        let stencilled = try colour(.depth32Float_stencil8, .type2D, 8, 8)
        let scratch = try gpu.texture(.rgba8Unorm, 8, 8)
        let multisampled = try gpu.texture(
            .rgba8Unorm, 8, 8, usage: [.renderTarget, .shaderRead, .pixelFormatView], samples: 4
        )
        let rows = try gpu.buffer([UInt8](Array(repeating: Self.code(3, 3), count: 64).joined()))
        let linearDescriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 8, height: 8, mipmapped: false
        )
        linearDescriptor.usage = [.shaderRead, .pixelFormatView]
        linearDescriptor.storageMode = .shared
        guard let linear = rows.makeTexture(descriptor: linearDescriptor, offset: 0, bytesPerRow: 32) else {
            return .fail("no texture over a buffer")
        }
        let fillPath = try await gpu.run { commands in
            func pass(_ configure: (MTLRenderPassDescriptor) -> Void) {
                let p = MTLRenderPassDescriptor()
                configure(p)
                commands.makeRenderCommandEncoder(descriptor: p)!.endEncoding()
            }
            func colourPass(_ t: MTLTexture, level: Int, slice: Int = 0, plane: Int = 0, code: [UInt8]) {
                pass { p in
                    p.colorAttachments[0].texture = t
                    p.colorAttachments[0].level = level
                    p.colorAttachments[0].slice = slice
                    p.colorAttachments[0].depthPlane = plane
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].clearColor = Self.clear(code)
                    p.colorAttachments[0].storeAction = .store
                }
            }
            for slice in 0 ..< 2 {
                for level in 0 ..< 5 { colourPass(array, level: level, slice: slice, code: Self.code(level, slice)) }
            }
            for slice in 0 ..< 12 { colourPass(cubes, level: 0, slice: slice, code: Self.code(0, slice)) }
            for level in 0 ..< 3 {
                for plane in 0 ..< max(1, 4 >> level) {
                    colourPass(volume, level: level, plane: plane, code: Self.code(level, plane))
                }
            }
            for slice in 0 ..< 3 {
                pass { p in
                    p.colorAttachments[0].texture = scratch
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].storeAction = .dontCare
                    p.depthAttachment.texture = depths
                    p.depthAttachment.slice = slice
                    p.depthAttachment.loadAction = .clear
                    p.depthAttachment.clearDepth = 0.25 * Double(slice + 1)
                    p.depthAttachment.storeAction = .store
                }
            }
            pass { p in
                p.colorAttachments[0].texture = scratch
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].storeAction = .dontCare
                p.depthAttachment.texture = stencilled
                p.depthAttachment.loadAction = .clear
                p.depthAttachment.storeAction = .store
                p.stencilAttachment.texture = stencilled
                p.stencilAttachment.loadAction = .clear
                p.stencilAttachment.clearStencil = 0x5a
                p.stencilAttachment.storeAction = .store
            }
            pass { p in
                p.colorAttachments[0].texture = multisampled
                p.colorAttachments[0].loadAction = .clear
                p.colorAttachments[0].clearColor = Self.clear(Self.code(4, 4))
                p.colorAttachments[0].storeAction = .store
            }
        }.path
        metrics.set("fill_path", fillPath)

        guard
            let levelSlice = array.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2D, levels: 1 ..< 2, slices: 1 ..< 2
            ),
            let cube = cubes.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .typeCube, levels: 0 ..< 1, slices: 6 ..< 12
            ),
            let volumeLevels = volume.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type3D, levels: 1 ..< 3, slices: 0 ..< 1
            ),
            let depthSlice = depths.makeTextureView(
                pixelFormat: .depth32Float, textureType: .type2D, levels: 0 ..< 1, slices: 2 ..< 3
            ),
            let depthLayers = depths.makeTextureView(
                pixelFormat: .depth32Float, textureType: .type2DArray, levels: 0 ..< 1, slices: 1 ..< 3
            ),
            let stencil = stencilled.makeTextureView(pixelFormat: .x32_stencil8),
            let linearArray = linear.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2DArray, levels: 0 ..< 1, slices: 0 ..< 1
            ),
            let multisampledArray = multisampled.makeTextureView(
                pixelFormat: .rgba8Unorm, textureType: .type2DMultisampleArray, levels: 0 ..< 1, slices: 0 ..< 1
            )
        else { return .fail("a view returned nil", metrics) }
        metrics.set("cube_tic", Self.header(gpu.device, cube))
        metrics.set("cubes_tic", Self.header(gpu.device, cubes))

        struct Arm {
            let name: String
            let view: MTLTexture
            let fragment: String
            let format: MTLPixelFormat
            let width: Int
            let expected: [[UInt8]]? // per pixel, rgba8 arms
            let floats: [Float]? // per pixel's red, r32Float arms
        }
        let arms = [
            Arm(
                name: "level_slice", view: levelSlice, fragment: "view_read_2d", format: .rgba8Unorm, width: 8,
                expected: Array(repeating: Self.code(1, 1), count: 8), floats: nil
            ),
            Arm(
                name: "cube", view: cube, fragment: "view_read_cube_face", format: .rgba8Unorm, width: 6,
                expected: (6 ..< 12).map { Self.code(0, $0) }, floats: nil
            ),
            Arm(
                name: "volume_levels", view: volumeLevels, fragment: "view_read_volume", format: .rgba8Unorm, width: 3,
                expected: [Self.code(1, 0), Self.code(1, 1), Self.code(2, 0)], floats: nil
            ),
            Arm(
                name: "depth_slice", view: depthSlice, fragment: "view_read_depth", format: .r32Float, width: 8,
                expected: nil, floats: Array(repeating: 0.75, count: 8)
            ),
            Arm(
                name: "depth_layers", view: depthLayers, fragment: "view_read_depth_layers", format: .r32Float,
                width: 2, expected: nil, floats: [0.5, 0.75]
            ),
            Arm(
                name: "stencil", view: stencil, fragment: "view_read_stencil", format: .r32Float, width: 8,
                expected: nil, floats: Array(repeating: 90, count: 8)
            ),
            Arm(
                name: "buffer_array", view: linearArray, fragment: "view_read_array_layer", format: .rgba8Unorm,
                width: 8, expected: Array(repeating: Self.code(3, 3), count: 8), floats: nil
            ),
            Arm(
                name: "multisample_array", view: multisampledArray, fragment: "view_read_multisample_layer",
                format: .rgba8Unorm, width: 8, expected: Array(repeating: Self.code(4, 4), count: 8), floats: nil
            ),
        ]
        for arm in arms {
            do {
                let target = try gpu.texture(arm.format, arm.width, 1)
                let state = try gpu.render(fragment: arm.fragment, format: arm.format)
                let path = try await gpu.run { commands in
                    let p = MTLRenderPassDescriptor()
                    p.colorAttachments[0].texture = target
                    p.colorAttachments[0].loadAction = .clear
                    p.colorAttachments[0].storeAction = .store
                    let e = commands.makeRenderCommandEncoder(descriptor: p)!
                    e.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    e.setFragmentTexture(arm.view, index: 0)
                    e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    e.endEncoding()
                }.path
                paths.append(path)
                metrics.set("\(arm.name)_path", path)
                var bad = 0
                var first = ""
                if let expected = arm.expected {
                    let bytes = try await gpu.read(target, bytesPerPixel: 4)
                    first = bytes[0 ..< 4].map { String($0) }.joined(separator: "/")
                    for x in 0 ..< arm.width where Array(bytes[4 * x ..< 4 * x + 4]) != expected[x] { bad += 1 }
                } else if let floats = arm.floats {
                    let bytes = try await gpu.read(target, bytesPerPixel: 4)
                    let values = (0 ..< arm.width).map { bytes.loaded(as: Float.self, at: $0) }
                    first = String(values[0])
                    for x in 0 ..< arm.width where values[x] != floats[x] { bad += 1 }
                }
                metrics.set("\(arm.name)_wrong", bad)
                metrics.set("\(arm.name)_first", first)
                if bad > 0 { wrong.append(arm.name) }
            } catch {
                metrics.set("\(arm.name)_error", "\(error)")
                wrong.append(arm.name)
            }
        }
        if paths.contains(where: { $0 != "gm20b" }) { wrong.append("paths") }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }

    static let headerSelector = NSSelectorFromString("switchMetalProbeHeader:")

    /// `texture`'s header words, as switchMetalProbeHeader: answers them; "-" where the device has no answer
    static func header(_ device: MTLDevice, _ texture: MTLTexture) -> String {
        guard let facade = device as? NSObject, facade.responds(to: headerSelector) else { return "-" }
        return facade.perform(headerSelector, with: texture)?.takeUnretainedValue() as? String ?? "-"
    }
}
