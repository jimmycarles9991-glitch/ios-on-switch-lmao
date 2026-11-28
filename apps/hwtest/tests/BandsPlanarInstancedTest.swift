import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// The Maps widget's striped render (25 Nov, c38-audit/perf1/maps-1125 NOTES section A): WidgetRenderer draws the widget
/// into an RGB8_444_2P (542) '_RB4' surface, through the target's RGBA16Float stand-in, with one instanced draw (17
/// instances) that samples a BGRA image and reads the colour it draws over (an interlocked, storage-form state), and on
/// the device that draw came out as seven horizontal stripes, one a band. Here the same shape of draw goes over a
/// 352x160 '_RB4' surface: whole (band budget 0), then in bands sized from the whole draw's GPU time to give about 4, 7
/// and 14 bands. Each banded arm's planes must be the whole draw's byte for byte. Metrics per arm n: bands_<n> (bands
/// drawn), identical_<n>, rows_off_<n> (luma rows that differ, a band's worth of rows per wrong band), path_<n>; and
/// us_whole, interlock_whole (the whole draw ran interlocked).
struct BandsPlanarInstancedTest: HWTest {
    let id = "gpu.sched.band_planar_instanced"
    let name = "An instanced draw sampling a BGRA image into an RGB8_444_2P target: banded as drawn whole"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    static let width = 352, height = 160, instances = 17, imageWidth = 382, imageHeight = 176
    static let format = MTLPixelFormat(rawValue: 542)!
    static let arms = [4, 7, 14]

    struct Drawn {
        let luma: [UInt8]
        let chroma: [UInt8]
        let bands: Int
        let interlocked: Int
        let timing: GPU.Timing
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard let production = DrawBandsTest.budget(gpu.device, 0) else {
            return .skip(GM20BFacade.missing("switchMetalBandBudget:", on: gpu.device))
        }
        defer { DrawBandsTest.budget(gpu.device, production) }
        var metrics = Metrics()
        var failures: [String] = []
        let pipeline = try gpu.render(
            vertex: "bands_instanced_vertex", fragment: "bands_sample_over_fragment", format: Self.format
        )
        let image = try Self.image(gpu)
        let whole = try await Self.draw(gpu, pipeline, image)
        metrics.set("us_whole", whole.timing.gpuMilliseconds * 1000, digits: 0)
        metrics.set("interlock_whole", whole.interlocked)
        metrics.set("bands_whole", whole.bands)
        if whole.bands != 0 { failures.append("\(whole.bands) bands with the budget at 0") }
        guard whole.timing.gpuTimed else {
            return .fail("the whole draw has no GPU time to size its bands from", metrics)
        }
        for n in Self.arms {
            DrawBandsTest.budget(gpu.device, max(1, Int(whole.timing.gpuMilliseconds * 1_000_000) / n))
            let banded = try await Self.draw(gpu, pipeline, image)
            DrawBandsTest.budget(gpu.device, 0)
            let identical = banded.luma == whole.luma && banded.chroma == whole.chroma
            metrics.set("bands_\(n)", banded.bands)
            metrics.set("identical_\(n)", identical)
            metrics.set("rows_off_\(n)", Self.rowsOff(banded.luma, whole.luma))
            metrics.set("path_\(n)", banded.timing.path)
            if banded.bands < 2 { failures.append("arm \(n): drawn in \(banded.bands) bands") }
            if !identical { failures.append("arm \(n): \(banded.bands) bands differ from the whole draw") }
        }
        return failures.isEmpty ? .pass(metrics) : .fail(failures.joined(separator: "; "), metrics)
    }

    /// A BGRA image of the snapshot's size with a different value at every texel, so a band sampling the wrong rows shows
    static func image(_ gpu: GPU) throws -> MTLTexture {
        let texture = try gpu.texture(
            .bgra8Unorm, imageWidth, imageHeight, usage: [.shaderRead], storage: .shared
        )
        var bytes = [UInt8](repeating: 0, count: imageWidth * imageHeight * 4)
        for y in 0 ..< imageHeight {
            for x in 0 ..< imageWidth {
                let at = (y * imageWidth + x) * 4
                bytes[at] = UInt8(truncatingIfNeeded: x * 3 + y)
                bytes[at + 1] = UInt8(truncatingIfNeeded: y * 5)
                bytes[at + 2] = UInt8(truncatingIfNeeded: x ^ (y << 1))
                bytes[at + 3] = 255
            }
        }
        texture.replace(
            region: MTLRegionMake2D(0, 0, imageWidth, imageHeight), mipmapLevel: 0, withBytes: bytes,
            bytesPerRow: imageWidth * 4
        )
        return texture
    }

    /// A two-plane '_RB4' surface of the widget snapshot's size, plane 1 at the next 4 KiB
    static func surface() throws -> IOSurface {
        let lumaBytes = width * height, chromaOffset = 4096 * ((lumaBytes + 4095) / 4096)
        let plane = { (bytes: Int, offset: Int) -> [String: Any] in
            [
                IOSurfacePropertyKey.planeWidth.rawValue: width, IOSurfacePropertyKey.planeHeight.rawValue: height,
                IOSurfacePropertyKey.planeBytesPerElement.rawValue: bytes,
                IOSurfacePropertyKey.planeBytesPerRow.rawValue: width * bytes,
                IOSurfacePropertyKey.planeOffset.rawValue: offset,
                IOSurfacePropertyKey.planeSize.rawValue: width * height * bytes,
            ]
        }
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: width, .height: height, .pixelFormat: OSType(0x5F52_4234),
            .planeInfo: [plane(1, 0), plane(2, chromaOffset)], .allocSize: chromaOffset + 2 * width * height,
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("no '_RB4' surface") }
        return surface
    }

    static func draw(_ gpu: GPU, _ pipeline: MTLRenderPipelineState, _ image: MTLTexture) async throws -> Drawn {
        let surface = try surface()
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: format, width: width, height: height, mipmapped: false
        )
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard
            let target = gpu.device.makeTexture(
                descriptor: descriptor, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0
            )
        else { throw TestFailure("no 542 texture over the '_RB4' surface") }
        let sampler = gpu.device.makeSamplerState(descriptor: {
            let s = MTLSamplerDescriptor()
            s.minFilter = .linear
            s.magFilter = .linear
            return s
        }())
        let before = GM20BFacade.counters(of: gpu.device)
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.1, green: 0.2, blue: 0.3, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
            encoder.setRenderPipelineState(pipeline)
            encoder.setFragmentTexture(image, index: 0)
            encoder.setFragmentSamplerState(sampler, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3, instanceCount: instances)
            encoder.endEncoding()
        }
        let after = GM20BFacade.counters(of: gpu.device)
        surface.lock(options: .readOnly, seed: nil)
        defer { surface.unlock(options: .readOnly, seed: nil) }
        let ref = unsafeBitCast(surface, to: IOSurfaceRef.self)
        let luma = IOSurfaceGetBaseAddressOfPlane(ref, 0).assumingMemoryBound(to: UInt8.self)
        let chroma = IOSurfaceGetBaseAddressOfPlane(ref, 1).assumingMemoryBound(to: UInt8.self)
        return Drawn(
            luma: Array(UnsafeBufferPointer(start: luma, count: width * height)),
            chroma: Array(UnsafeBufferPointer(start: chroma, count: 2 * width * height)),
            bands: (after["bands"] ?? 0) - (before["bands"] ?? 0),
            interlocked: (after["interlock_draws"] ?? 0) - (before["interlock_draws"] ?? 0), timing: timing
        )
    }

    static func rowsOff(_ a: [UInt8], _ b: [UInt8]) -> Int {
        (0 ..< height).filter { y in a[(y * width) ..< ((y + 1) * width)] != b[(y * width) ..< ((y + 1) * width)] }
            .count
    }
}
