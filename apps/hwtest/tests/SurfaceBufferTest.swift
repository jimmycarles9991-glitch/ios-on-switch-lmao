import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// Two device methods the facade answers itself (drivers/TegraGM20BMetal/
/// TegraGM20BMetalDevice.mm newBufferWithIOSurface:, TegraGM20BMetalDeviceAnswers.mm requiredLinearTextureBytesPerRowForDescriptor:):
/// a buffer over an IOSurface's pages reads what the CPU wrote through the surface and is copied by the GPU, and a
/// linear row is the texels rounded up to the format's linear alignment, 0 for a format of blocks.
struct SurfaceBufferTest: HWTest {
    let id = "gpu.gm20b.surface_buffer"
    let name = "A buffer over an IOSurface and a linear texture's row, answered by the facade"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { throw TestFailure("the default device is not the facade (\(kind))") }
        let page = Int(vm_page_size)
        var metrics = Metrics()
        var wrong: [String] = []
        var off = 0, smallOff = 0, paddingOff = 0, nonPageAllocations = 0
        var path = "gm20b"
        for height in [64, 80, 96, 112] {
            let properties: [CFString: Any] = [
                kIOSurfaceWidth: 64, kIOSurfaceHeight: height, kIOSurfaceBytesPerElement: 4,
                kIOSurfaceBytesPerRow: 256, kIOSurfaceAllocSize: 256 * height,
                kIOSurfacePixelFormat: 0x4247_5241, // 'BGRA'
            ]
            guard let surface = IOSurfaceCreate(properties as CFDictionary) else {
                throw TestFailure("no IOSurface at height \(height)")
            }
            let bytes = IOSurfaceGetAllocSize(surface)
            let mapped = (bytes + page - 1) / page * page
            metrics.set("alloc_\(height)", bytes)
            metrics.set("mapped_\(height)", mapped)
            if bytes % page != 0 { nonPageAllocations += 1 }
            IOSurfaceLock(surface, [], nil)
            let base = IOSurfaceGetBaseAddress(surface).bindMemory(to: UInt32.self, capacity: bytes / 4)
            for i in 0 ..< bytes / 4 { base[i] = UInt32(truncatingIfNeeded: i &* 2_654_435_761) }
            IOSurfaceUnlock(surface, [], nil)
            let made = (gpu.device as AnyObject).perform(NSSelectorFromString("newBufferWithIOSurface:"), with: surface)
            guard let surfaceBuffer = made?.takeRetainedValue() as? MTLBuffer else {
                throw TestFailure("newBufferWithIOSurface: returned nil at height \(height)")
            }
            metrics.set("buffer_length_\(height)", surfaceBuffer.length)
            if height == 64 { metrics.set("buffer_length", surfaceBuffer.length) }
            guard surfaceBuffer.length >= bytes && surfaceBuffer.length <= mapped else {
                throw TestFailure(
                    "height \(height): allocation \(bytes), mapped \(mapped), buffer \(surfaceBuffer.length)"
                )
            }
            if surfaceBuffer.length != mapped || surfaceBuffer.contents() != IOSurfaceGetBaseAddress(surface) {
                wrong.append("buffer_\(height)")
            }
            let copy = try gpu.buffer(surfaceBuffer.length)
            for count in [4, surfaceBuffer.length] {
                memset(copy.contents(), 0xA5, copy.length)
                let result = try await gpu.run { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    blit.copy(from: surfaceBuffer, sourceOffset: 0, to: copy, destinationOffset: 0, size: count)
                    blit.endEncoding()
                }
                if result.path != "gm20b" {
                    path = result.path
                    wrong.append("path_\(height)_\(count)")
                }
                let copied = copy.contents().bindMemory(to: UInt32.self, capacity: count / 4)
                for i in 0 ..< min(bytes, count) / 4 where copied[i] != UInt32(truncatingIfNeeded: i &* 2_654_435_761) {
                    off += 1
                    if count == 4 { smallOff += 1 }
                }
                if count > bytes {
                    let expected = surfaceBuffer.contents().bindMemory(to: UInt8.self, capacity: count)
                    let actual = copy.contents().bindMemory(to: UInt8.self, capacity: count)
                    for i in bytes ..< count where actual[i] != expected[i] { paddingOff += 1 }
                }
            }
        }
        let selector = NSSelectorFromString("requiredLinearTextureBytesPerRowForDescriptor:")
        typealias Row = @convention(c) (AnyObject, Selector, MTLTextureDescriptor) -> Int
        let row = unsafeBitCast((gpu.device as AnyObject).method(for: selector), to: Row.self)
        let linear = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: 100, height: 4, mipmapped: false
        )
        let align = gpu.device.minimumLinearTextureAlignment(for: .bgra8Unorm)
        let bgraRow = row(gpu.device as AnyObject, selector, linear)
        linear.pixelFormat = .astc_4x4_ldr
        let astcRow = row(gpu.device as AnyObject, selector, linear)
        metrics.set("copied_words_off", off)
        metrics.set("small_copy_words_off", smallOff)
        metrics.set("padding_bytes_off", paddingOff)
        metrics.set("non_page_allocations", nonPageAllocations)
        metrics.set("bgra8_row_100", bgraRow)
        metrics.set("astc_row", astcRow)
        metrics.set("path", path)
        let answer = NativeUnsupportedTest.answer(gpu.device, "supportsBufferWithIOSurface") ?? -1
        metrics.set("answer", answer)
        if off > 0 { wrong.append("copy") }
        if paddingOff > 0 { wrong.append("padding") }
        if nonPageAllocations == 0 { wrong.append("no_non_page_allocation") }
        if bgraRow != (400 + align - 1) / align * align { wrong.append("bgra8_row") }
        if astcRow != 0 { wrong.append("astc_row") }
        if path != "gm20b" { wrong.append("path") }
        if answer != 1 { wrong.append("answer") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
