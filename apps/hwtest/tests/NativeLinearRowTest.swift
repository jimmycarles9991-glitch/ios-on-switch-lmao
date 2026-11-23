import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A linear texture's row of a format the driver does not lay out (drivers/TegraGM20BMetal/
/// TegraGM20BMetalDeviceAnswers.mm requiredLinearTextureBytesPerRowForDescriptor:, metal_formats.h): its texels at Metal's
/// size rounded up to the format's linear alignment, 0 for a depth format, which has no linear alignment.
struct NativeLinearRowTest: HWTest {
    let id = "metal.native.linear_row"
    let name = "A linear row of a format raw does not lay out, answered by the device"
    let subsystem = Subsystem.metal

    static let width = 100
    /// Formats with a linear alignment and no raw layout, and Metal's bytes of their texel
    static let formats: [(name: String, format: MTLPixelFormat, bytes: Int)] = [
        ("r8snorm", .r8Snorm, 1), ("rg8snorm", .rg8Snorm, 2), ("rgba8snorm", .rgba8Snorm, 4),
        ("rgba16snorm", .rgba16Snorm, 8), ("rgba16bfloat", MTLPixelFormat(rawValue: 116)!, 8),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let selector = NSSelectorFromString("requiredLinearTextureBytesPerRowForDescriptor:")
        typealias Row = @convention(c) (AnyObject, Selector, MTLTextureDescriptor) -> Int
        let row = unsafeBitCast((gpu.device as AnyObject).method(for: selector), to: Row.self)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Snorm, width: Self.width, height: 4, mipmapped: false
        )
        var metrics = Metrics()
        var wrong: [String] = []
        for (name, format, bytes) in Self.formats {
            descriptor.pixelFormat = format
            let align = gpu.device.minimumLinearTextureAlignment(for: format)
            let answered = row(gpu.device as AnyObject, selector, descriptor)
            metrics.set("\(name)_row", answered)
            if align == 0 || answered != (Self.width * bytes + align - 1) / align * align { wrong.append(name) }
        }
        descriptor.pixelFormat = .depth32Float
        let depthRow = row(gpu.device as AnyObject, selector, descriptor)
        metrics.set("depth32f_row", depthRow)
        if depthRow != 0 { wrong.append("depth32f") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
