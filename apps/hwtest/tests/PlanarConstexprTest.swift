import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// RenderBox's widget snapshots on the raw path (boot 21 WidgetRenderer: enc.planarclamp a=0 b=2, then the command
/// buffer failed): an RGB8_444_2P (542) texture over a two-plane 4:4:4 '_RB4' surface sampled through constexpr samplers,
/// the quad's corners set as vertex bytes before the encoder's first draw. Metal reads its codes (red Cr, green Y, blue
/// Cb, alpha 1) and samples clamp-to-zero and clamp-to-border as clamp-to-edge (m2v texture/ycbcr_image.rs ycbcr_modes,
/// measured), the wrapping modes as themselves. Target pixel (x, y) samples texel (x - 1, y - 1), so the ring around the
/// texture reads each mode outside it. PASS: one raw command buffer, every pixel the texel its mode names.
struct PlanarConstexprTest: HWTest {
    let id = "gpu.raw.planar_constexpr"
    let name = "A two-plane 4:4:4 texture through constexpr samplers of each address mode, on the raw path"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let width = 13, height = 7, lumaRow = 64, chromaRow = 64
    static let rgb8_444_2P = MTLPixelFormat(rawValue: 542)!

    enum Outside { case edge, wrap, mirror }

    static let modes: [(fragment: String, label: String, outside: Outside)] = [
        ("planar_constexpr_zero", "zero", .edge), ("planar_constexpr_border", "border", .edge),
        ("planar_constexpr_edge", "edge", .edge), ("planar_constexpr_repeat", "repeat", .wrap),
        ("planar_constexpr_mirror", "mirror", .mirror), ("planar_constexpr_linear", "linear_zero", .edge),
    ]

    static func luma(_ x: Int, _ y: Int) -> UInt8 { UInt8((x * 7 + y * 13 + 1) & 255) }
    static func cb(_ x: Int, _ y: Int) -> UInt8 { UInt8((x * 3 + y * 5 + 40) & 255) }
    static func cr(_ x: Int, _ y: Int) -> UInt8 { UInt8((x * 11 + y * 2 + 90) & 255) }

    static func texel(_ i: Int, _ size: Int, _ outside: Outside) -> Int {
        switch outside {
        case .edge: return min(max(i, 0), size - 1)
        case .wrap: return (i + size) % size
        case .mirror: return i < 0 ? -i - 1 : i >= size ? 2 * size - i - 1 : i
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let surface = try Self.surface()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: Self.rgb8_444_2P, width: Self.width, height: Self.height, mipmapped: false)
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard
            let image = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0)
        else { throw TestFailure("makeTexture(iosurface:) RGB8_444_2P returned nil") }
        let (w, h) = (Self.width + 2, Self.height + 2)
        var states: [MTLRenderPipelineState] = []
        var targets: [MTLTexture] = []
        for mode in Self.modes {
            states.append(try gpu.render(fragment: mode.fragment, format: .rgba8Unorm))
            targets.append(try gpu.texture(.rgba8Unorm, w, h))
        }
        let path = try await gpu.run { commands in
            for (state, target) in zip(states, targets) {
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentTexture(image, index: 0)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
        }.path
        var metrics = Metrics()
        var wrong: [String] = []
        metrics.set("path", path)
        if path != "raw" { wrong.append("path") }
        for (mode, target) in zip(Self.modes, targets) {
            let bytes = try await gpu.read(target, bytesPerPixel: 4)
            var off = 0
            for y in 0..<h {
                for x in 0..<w {
                    let tx = Self.texel(x - 1, Self.width, mode.outside)
                    let ty = Self.texel(y - 1, Self.height, mode.outside)
                    let want = [Self.cr(tx, ty), Self.luma(tx, ty), Self.cb(tx, ty), 255]
                    let at = 4 * (x + w * y)
                    off += zip(bytes[at..<at + 4], want).contains { abs(Int($0) - Int($1)) > 1 } ? 1 : 0
                }
            }
            metrics.set("\(mode.label)_wrong", off)
            if off > 0 { wrong.append(mode.label) }
        }
        return wrong.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// '_RB4': luma codes in plane 0, Cb and Cr pairs in plane 1, both at the texture's size
    static func surface() throws -> IOSurface {
        let lumaBytes = lumaRow * height, chromaOffset = (lumaBytes + 4095) / 4096 * 4096
        let chromaBytes = chromaRow * height
        let planes: [[String: Any]] = [
            [
                IOSurfacePropertyKey.planeWidth.rawValue: width, IOSurfacePropertyKey.planeHeight.rawValue: height,
                IOSurfacePropertyKey.planeBytesPerElement.rawValue: 1,
                IOSurfacePropertyKey.planeBytesPerRow.rawValue: lumaRow, IOSurfacePropertyKey.planeOffset.rawValue: 0,
                IOSurfacePropertyKey.planeSize.rawValue: lumaBytes,
            ],
            [
                IOSurfacePropertyKey.planeWidth.rawValue: width, IOSurfacePropertyKey.planeHeight.rawValue: height,
                IOSurfacePropertyKey.planeBytesPerElement.rawValue: 2,
                IOSurfacePropertyKey.planeBytesPerRow.rawValue: chromaRow,
                IOSurfacePropertyKey.planeOffset.rawValue: chromaOffset,
                IOSurfacePropertyKey.planeSize.rawValue: chromaBytes,
            ],
        ]
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .pixelFormat: OSType(0x5F52_4234), .planeInfo: planes,
            .allocSize: (chromaOffset + chromaBytes + 16383) / 16384 * 16384,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("no '_RB4' surface") }
        surface.lock(options: [], seed: nil)
        let ref = unsafeBitCast(surface, to: IOSurfaceRef.self)
        let y = IOSurfaceGetBaseAddressOfPlane(ref, 0).assumingMemoryBound(to: UInt8.self)
        let cbcr = IOSurfaceGetBaseAddressOfPlane(ref, 1).assumingMemoryBound(to: UInt8.self)
        for row in 0..<height {
            for x in 0..<width {
                y[row * lumaRow + x] = luma(x, row)
                cbcr[row * chromaRow + 2 * x] = cb(x, row)
                cbcr[row * chromaRow + 2 * x + 1] = cr(x, row)
            }
        }
        surface.unlock(options: [], seed: nil)
        return surface
    }
}
