import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// fillTexture:level:slice:region:color: (SPI) on the raw path: a region of a 64x64 texture set to one colour, drawn as
/// CLEAR_SURFACE over the region in a pass of its own (raw_twod_fill.h; the 2D engine's solid rectangle where rule
/// clear.twod holds for the format), per format. PASS: every format's fill on the raw path, the region at Metal's codes of
/// the colour (sRGB channels encoded), the texels around it as uploaded.
struct FillTextureTest: HWTest {
    let id = "gpu.raw.fill_texture"
    let name = "fillTexture: on the raw path: the region at the colour's codes, the rest untouched"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Fill = @convention(c) (AnyObject, Selector, MTLTexture, Int, Int, MTLRegion, MTLClearColor) -> Void
    static let selector = NSSelectorFromString("fillTexture:level:slice:region:color:")
    static let side = 64
    static let region = MTLRegionMake2D(5, 7, 40, 31)
    static let colour = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.8)

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for (name, format) in [
            ("bgra8", MTLPixelFormat.bgra8Unorm), ("rgba8", .rgba8Unorm), ("bgra8srgb", .bgra8Unorm_srgb),
        ] {
            let texture = try gpu.texture(format, Self.side, Self.side, usage: [.renderTarget, .shaderRead])
            let pattern = (0..<Self.side * Self.side * 4).map { UInt8(truncatingIfNeeded: $0 &* 37) }
            try await gpu.upload(pattern, to: texture, bytesPerRow: Self.side * 4)
            var responded = true
            let path = try await gpu.run { commands in
                let blit = commands.makeBlitCommandEncoder()!
                if let object = blit as AnyObject?, object.responds(to: Self.selector) {
                    unsafeBitCast(object.method(for: Self.selector), to: Fill.self)(
                        object, Self.selector, texture, 0, 0, Self.region, Self.colour)
                } else {
                    responded = false
                }
                blit.endEncoding()
            }.path
            guard responded else { return .skip("the blit encoder answers no fillTexture:") }
            let got = try await gpu.read(texture, bytesPerPixel: 4)
            let want = Self.codes(format)
            var inside = 0, outside = 0
            for y in 0..<Self.side {
                for x in 0..<Self.side {
                    let at = (y * Self.side + x) * 4
                    let inRegion = x >= 5 && x < 45 && y >= 7 && y < 38
                    for c in 0..<4 {
                        if inRegion && abs(Int(got[at + c]) - Int(want[c])) > 0 { inside += 1 }
                        if !inRegion && got[at + c] != pattern[at + c] { outside += 1 }
                    }
                }
            }
            metrics.set("\(name)_path", path)
            metrics.set("\(name)_inside_wrong", inside)
            metrics.set("\(name)_outside_changed", outside)
            if path != "raw" { wrong.append("\(name)_path") }
            if inside > 0 || outside > 0 { wrong.append(name) }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The colour's bytes in memory order, Metal's nearest codes, sRGB channels encoded
    static func codes(_ format: MTLPixelFormat) -> [UInt8] {
        let srgb = format == .bgra8Unorm_srgb
        func code(_ v: Double, _ encode: Bool) -> UInt8 {
            let e = encode ? (v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055) : v
            return UInt8(min(max(e, 0), 1) * 255 + 0.5)
        }
        let r = code(colour.red, srgb), g = code(colour.green, srgb), b = code(colour.blue, srgb)
        let a = code(colour.alpha, false)
        return format == .rgba8Unorm ? [r, g, b, a] : [b, g, r, a]
    }
}
