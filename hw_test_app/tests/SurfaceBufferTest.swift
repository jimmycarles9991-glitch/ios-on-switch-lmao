import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal

/// Two device methods the raw facade answers itself (drivers/TegraGM20BMetal/raw/runtime/
/// RawMetalDevice.mm newBufferWithIOSurface:, RawMetalDeviceAnswers.mm requiredLinearTextureBytesPerRowForDescriptor:):
/// a buffer over an IOSurface's pages reads what the CPU wrote through the surface and is copied by the GPU, and a
/// linear row is the texels rounded up to the format's linear alignment, 0 for a format of blocks.
struct SurfaceBufferTest: HWTest {
    let id = "gpu.raw.surface_buffer"
    let name = "A buffer over an IOSurface and a linear texture's row, answered by the facade"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let properties: [CFString: Any] = [
            kIOSurfaceWidth: 64, kIOSurfaceHeight: 64, kIOSurfaceBytesPerElement: 4,
            kIOSurfacePixelFormat: 0x4247_5241,  // 'BGRA'
        ]
        guard let surface = IOSurfaceCreate(properties as CFDictionary) else { throw TestFailure("no IOSurface") }
        let bytes = IOSurfaceGetAllocSize(surface)
        IOSurfaceLock(surface, [], nil)
        let base = IOSurfaceGetBaseAddress(surface).bindMemory(to: UInt32.self, capacity: bytes / 4)
        for i in 0..<bytes / 4 { base[i] = UInt32(truncatingIfNeeded: i &* 2_654_435_761) }
        IOSurfaceUnlock(surface, [], nil)
        let made = (gpu.device as AnyObject).perform(NSSelectorFromString("newBufferWithIOSurface:"), with: surface)
        guard let surfaceBuffer = made?.takeRetainedValue() as? MTLBuffer else {
            throw TestFailure("newBufferWithIOSurface: returned nil")
        }
        let copy = try gpu.buffer(bytes)
        let path = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: surfaceBuffer, sourceOffset: 0, to: copy, destinationOffset: 0, size: bytes)
            blit.endEncoding()
        }.path
        let copied = copy.contents().bindMemory(to: UInt32.self, capacity: bytes / 4)
        var off = 0
        for i in 0..<bytes / 4 where copied[i] != UInt32(truncatingIfNeeded: i &* 2_654_435_761) { off += 1 }
        let selector = NSSelectorFromString("requiredLinearTextureBytesPerRowForDescriptor:")
        typealias Row = @convention(c) (AnyObject, Selector, MTLTextureDescriptor) -> Int
        let row = unsafeBitCast((gpu.device as AnyObject).method(for: selector), to: Row.self)
        let linear = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 100, height: 4, mipmapped: false)
        let align = gpu.device.minimumLinearTextureAlignment(for: .bgra8Unorm)
        let bgraRow = row(gpu.device as AnyObject, selector, linear)
        linear.pixelFormat = .astc_4x4_ldr
        let astcRow = row(gpu.device as AnyObject, selector, linear)
        var metrics = Metrics()
        metrics.set("buffer_length", surfaceBuffer.length)
        metrics.set("copied_words_off", off)
        metrics.set("bgra8_row_100", bgraRow)
        metrics.set("astc_row", astcRow)
        metrics.set("path", path)
        let answer = NativeUnsupportedTest.answer(gpu.device, "supportsBufferWithIOSurface") ?? -1
        metrics.set("answer", answer)
        var wrong: [String] = []
        if surfaceBuffer.length < bytes || surfaceBuffer.contents() != IOSurfaceGetBaseAddress(surface) {
            wrong.append("buffer")
        }
        if off > 0 { wrong.append("copy") }
        if bgraRow != (400 + align - 1) / align * align { wrong.append("bgra8_row") }
        if astcRow != 0 { wrong.append("astc_row") }
        if path != "raw" { wrong.append("path") }
        if answer != 1 { wrong.append("answer") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
