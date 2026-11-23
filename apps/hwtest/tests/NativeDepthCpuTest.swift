import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// replaceRegion and getBytes of private one-aspect depth and stencil textures (drivers/TegraGM20BMetal/
/// gpu.cpp transferTexture): the bytes written read back, through the copy engine. Apple's driver does the same
/// with API validation off.
struct NativeDepthCpuTest: HWTest {
    let id = "metal.native.depth_cpu"
    let name = "CPU copies of private depth and stencil textures"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        let size = 64
        for (name, format, bpp) in [
            ("depth32f", MTLPixelFormat.depth32Float, 4), ("depth16", .depth16Unorm, 2), ("stencil8", .stencil8, 1),
        ] {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: format, width: size, height: size, mipmapped: false
            )
            descriptor.storageMode = .private
            descriptor.usage = [.renderTarget, .shaderRead]
            guard let texture = gpu.device.makeTexture(descriptor: descriptor) else {
                wrong.append("\(name)_made")
                continue
            }
            let region = MTLRegionMake2D(0, 0, size, size)
            var written = [UInt8](repeating: 0, count: size * size * bpp)
            for i in written.indices { written[i] = UInt8(truncatingIfNeeded: i &* 37 &+ 11) }
            if format == .depth32Float {
                written.withUnsafeMutableBytes { raw in
                    let floats = raw.bindMemory(to: Float.self)
                    for i in floats.indices { floats[i] = Float(i % 1000) / 1000 }
                }
            }
            texture.replace(region: region, mipmapLevel: 0, withBytes: written, bytesPerRow: size * bpp)
            var read = [UInt8](repeating: 0xab, count: written.count)
            texture.getBytes(&read, bytesPerRow: size * bpp, from: region, mipmapLevel: 0)
            let off = zip(read, written).filter { $0 != $1 }.count
            metrics.set("\(name)_bytes_off", off)
            if off != 0 { wrong.append(name) }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
