import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A linear texture's row of a format the raw path does not lay out (drivers/TegraGM20BMetal/raw/runtime/
/// RawMetalDeviceAnswers.mm requiredLinearTextureBytesPerRowForDescriptor:, raw_metal_formats.h): its texels at Metal's
/// size rounded up to the format's linear alignment, 0 for a depth format, which has no linear alignment, with no m2v
/// device asked.
struct NativeLinearRowTest: HWTest {
    let id = "metal.native.linear_row"
    let name = "A linear row of a format raw does not lay out, answered without m2v"
    let subsystem = Subsystem.metal

    static let width = 100
    /// Formats with a linear alignment and no raw layout, and Metal's bytes of their texel
    static let formats: [(name: String, format: MTLPixelFormat, bytes: Int)] = [
        ("r8snorm", .r8Snorm, 1), ("rg8snorm", .rg8Snorm, 2), ("rgba8snorm", .rgba8Snorm, 4),
        ("rgba16snorm", .rgba16Snorm, 8), ("rgba16bfloat", MTLPixelFormat(rawValue: 116)!, 8),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let needsBefore = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let selector = NSSelectorFromString("requiredLinearTextureBytesPerRowForDescriptor:")
        typealias Row = @convention(c) (AnyObject, Selector, MTLTextureDescriptor) -> Int
        let row = unsafeBitCast((gpu.device as AnyObject).method(for: selector), to: Row.self)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .r8Snorm, width: Self.width, height: 4, mipmapped: false)
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
        let needsAfter = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let asked = Set(needsAfter.keys).subtracting(needsBefore.keys).sorted()
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
