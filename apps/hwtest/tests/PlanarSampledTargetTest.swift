import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// An RGB8_444_2P (542) texture made with usage ShaderRead alone over a 64x64 '_RB4' surface, rendered to as a target:
/// a pass that clears to (0.5, 0.25, 0.75, 1) and stores with no draw, then a pass that loads, draws a 16x16 quad of
/// (1, 0, 0, 1) at (8, 8) and stores, each its own command buffer; the same into a twin surface's 542 made with
/// ShaderRead|RenderTarget. An M4 Max renders any usage as a target (m2v-textures5 probes): both command buffers
/// complete and the two surfaces' planes are byte for byte the same, and the twin holds Apple's bytes (apple_bytes_off:
/// green in plane 0, blue and red pairs in plane 1; the clear 40 / bf 80, the quad 00 / 00 ff). status: command buffers that failed
/// (0: completed); luma_bytes_differ, chroma_bytes_differ: bytes the twins' planes differ by; path: the ShaderRead
/// texture's command buffers' paths. '_RB4' surfaces are made through IOSurface, as gpu.gm20b.planar_constexpr's: Core
/// Video makes no '_RB4' pixel buffer.
struct PlanarSampledTargetTest: HWTest {
    let id = "gpu.gm20b.planar_sampled_target"
    let name = "An RGB8_444_2P made ShaderRead alone renders as its ShaderRead|RenderTarget twin does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let side = 64, quad = (8, 8, 24, 24)
    static let format = MTLPixelFormat(rawValue: 542)!
    static let clear = MTLClearColor(red: 0.5, green: 0.25, blue: 0.75, alpha: 1)
    static let apple = (
        clear: (green: UInt8(0x40), blue: UInt8(0xbf), red: UInt8(0x80)),
        quad: (green: UInt8(0), blue: UInt8(0), red: UInt8(0xff))
    )

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let sampled = try await Self.drawn(gpu, usage: [.shaderRead])
            let target = try await Self.drawn(gpu, usage: [.shaderRead, .renderTarget])
            let failed = (sampled.paths + target.paths).filter { $0.hasPrefix("error:") }
            metrics.set("status", failed.count)
            metrics.set("luma_bytes_differ", zip(sampled.luma, target.luma).filter { $0 != $1 }.count)
            metrics.set("chroma_bytes_differ", zip(sampled.chroma, target.chroma).filter { $0 != $1 }.count)
            metrics.set("apple_bytes_off", Self.offApple(target))
            metrics.set("path", Set(sampled.paths).sorted().joined(separator: "/"))
            if let first = failed.first { metrics.set("failed", first) }
            checks.append(("both command buffers of each surface complete", failed.isEmpty))
            checks.append(
                (
                    "the ShaderRead texture's planes are its twin's",
                    sampled.luma == target.luma && sampled.chroma == target.chroma
                )
            )
            checks.append(("the twin holds Apple's bytes", metrics["apple_bytes_off"] == .integer(0)))
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    struct Drawn {
        let paths: [String]
        let luma: [UInt8]
        let chroma: [UInt8]
    }

    static func inside(_ x: Int, _ y: Int) -> Bool { x >= quad.0 && x < quad.2 && y >= quad.1 && y < quad.3 }

    static func offApple(_ drawn: Drawn) -> Int {
        var off = 0
        for y in 0 ..< side {
            for x in 0 ..< side {
                let want = inside(x, y) ? apple.quad : apple.clear
                let at = y * side + x
                off += drawn.luma[at] == want.green ? 0 : 1
                off += drawn.chroma[2 * at] == want.blue && drawn.chroma[2 * at + 1] == want.red ? 0 : 1
            }
        }
        return off
    }

    /// A two-plane '_RB4' surface of side x side, plane 1 at the next 4 KiB, every byte i * 31 + 7
    static func surface() throws -> IOSurface {
        let lumaBytes = side * side, chromaOffset = 4096 * ((lumaBytes + 4095) / 4096)
        let plane = { (bytes: Int, offset: Int) -> [String: Any] in
            [
                IOSurfacePropertyKey.planeWidth.rawValue: side, IOSurfacePropertyKey.planeHeight.rawValue: side,
                IOSurfacePropertyKey.planeBytesPerElement.rawValue: bytes,
                IOSurfacePropertyKey.planeBytesPerRow.rawValue: side * bytes,
                IOSurfacePropertyKey.planeOffset.rawValue: offset,
                IOSurfacePropertyKey.planeSize.rawValue: side * side * bytes,
            ]
        }
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .pixelFormat: OSType(0x5F52_4234),
            .planeInfo: [plane(1, 0), plane(2, chromaOffset)], .allocSize: chromaOffset + 2 * side * side,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("no '_RB4' surface") }
        surface.lock(options: [], seed: nil)
        let base = surface.baseAddress.assumingMemoryBound(to: UInt8.self)
        for i in 0 ..< surface.allocationSize { base[i] = UInt8(truncatingIfNeeded: i * 31 + 7) }
        surface.unlock(options: [], seed: nil)
        return surface
    }

    static func drawn(_ gpu: GPU, usage: MTLTextureUsage) async throws -> Drawn {
        let surface = try surface()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: side, height: side, mipmapped: false
        )
        descriptor.usage = usage
        descriptor.storageMode = .shared
        guard
            let texture = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0
            )
        else { throw TestFailure("no 542 texture over the '_RB4' surface (usage \(usage.rawValue))") }
        let state = try gpu.render(fragment: "solid_fragment", format: format)
        var paths: [String] = []
        for load in [MTLLoadAction.clear, .load] {
            paths.append(
                try await gpu.run(tolerateError: true) { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = texture
                    pass.colorAttachments[0].loadAction = load
                    pass.colorAttachments[0].clearColor = clear
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    if load == .load {
                        encoder.setRenderPipelineState(state)
                        GPU.draw(
                            encoder, corners: GPU.pixelQuad(quad.0, quad.1, quad.2, quad.3, width: side, height: side),
                            color: SIMD4(1, 0, 0, 1)
                        )
                    }
                    encoder.endEncoding()
                }.path
            )
        }
        surface.lock(options: .readOnly, seed: nil)
        defer { surface.unlock(options: .readOnly, seed: nil) }
        let ref = unsafeBitCast(surface, to: IOSurfaceRef.self)
        let luma = IOSurfaceGetBaseAddressOfPlane(ref, 0).assumingMemoryBound(to: UInt8.self)
        let chroma = IOSurfaceGetBaseAddressOfPlane(ref, 1).assumingMemoryBound(to: UInt8.self)
        return Drawn(
            paths: paths, luma: Array(UnsafeBufferPointer(start: luma, count: side * side)),
            chroma: Array(UnsafeBufferPointer(start: chroma, count: 2 * side * side))
        )
    }
}
