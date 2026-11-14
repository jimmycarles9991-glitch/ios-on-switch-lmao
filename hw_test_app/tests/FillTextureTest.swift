import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// fillTexture:level:slice:region:color: (SPI) on the raw path: a region of a 64x64 texture set to one colour, drawn by
/// the 2D engine's solid rectangle in the target's own format where rule clear.twod holds for it (raw_twod_fill.h: plain
/// memory, the formats whose gpu.x1.twod_clear cells read exact), else as CLEAR_SURFACE over the region in a pass of its
/// own. Each format's target is made on plain memory (switchRawProbePlainPrivate:, where the facade has it) so the 2D
/// route is the one judged. PASS: every format's fill on the raw path, the region at Metal's codes of the colour (sRGB
/// channels encoded), the texels around it as uploaded, and each 2D format's fill by the 2D engine (`<fmt>_twod`, the
/// facade's fill_twod counter).
struct FillTextureTest: HWTest {
    let id = "gpu.raw.fill_texture"
    let name = "fillTexture: on the raw path: the region at the colour's codes, the rest untouched"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Fill = @convention(c) (AnyObject, Selector, MTLTexture, Int, Int, MTLRegion, MTLClearColor) -> Void
    static let selector = NSSelectorFromString("fillTexture:level:slice:region:color:")
    static let plainSelector = NSSelectorFromString("switchRawProbePlainPrivate:")
    static let side = 64
    static let region = MTLRegionMake2D(5, 7, 40, 31)
    static let colour = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.8)

    struct Cell {
        let name: String
        let format: MTLPixelFormat
        let bytes: Int
    }

    static let cells = [
        Cell(name: "bgra8", format: .bgra8Unorm, bytes: 4),
        Cell(name: "rgba8", format: .rgba8Unorm, bytes: 4),
        Cell(name: "bgra8srgb", format: .bgra8Unorm_srgb, bytes: 4),
        Cell(name: "rgba8srgb", format: .rgba8Unorm_srgb, bytes: 4),
        Cell(name: "rgb10a2", format: .rgb10a2Unorm, bytes: 4),
        Cell(name: "r8", format: .r8Unorm, bytes: 1),
        Cell(name: "r16", format: .r16Unorm, bytes: 2),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let facade = gpu.device as? NSObject
        let plain = facade?.responds(to: Self.plainSelector) ?? false
        var metrics = Metrics()
        var wrong: [String] = []
        for cell in Self.cells {
            if plain { _ = facade?.perform(Self.plainSelector, with: NSNumber(value: true)) }
            let made = Result {
                try gpu.texture(cell.format, Self.side, Self.side, usage: [.renderTarget, .shaderRead])
            }
            if plain { _ = facade?.perform(Self.plainSelector, with: NSNumber(value: false)) }
            let texture = try made.get()
            let pattern = (0..<Self.side * Self.side * cell.bytes).map { UInt8(truncatingIfNeeded: $0 &* 37) }
            try await gpu.upload(pattern, to: texture, bytesPerRow: Self.side * cell.bytes)
            let before = RawFacade.counters(of: gpu.device)["fill_twod"]
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
            let after = RawFacade.counters(of: gpu.device)["fill_twod"]
            let twod = before.flatMap { b in after.map { $0 - b } }
            let got = try await gpu.read(texture, bytesPerPixel: cell.bytes)
            let want = Self.codes(cell)
            var inside = 0, outside = 0
            for y in 0..<Self.side {
                for x in 0..<Self.side {
                    let at = (y * Self.side + x) * cell.bytes
                    let inRegion = x >= 5 && x < 45 && y >= 7 && y < 38
                    for c in 0..<cell.bytes {
                        if inRegion && got[at + c] != want[c] { inside += 1 }
                        if !inRegion && got[at + c] != pattern[at + c] { outside += 1 }
                    }
                }
            }
            metrics.set("\(cell.name)_path", path)
            metrics.set("\(cell.name)_inside_wrong", inside)
            metrics.set("\(cell.name)_outside_changed", outside)
            if let twod { metrics.set("\(cell.name)_twod", twod) }
            if path != "raw" { wrong.append("\(cell.name)_path") }
            if inside > 0 || outside > 0 { wrong.append(cell.name) }
            if plain, let twod, twod != 1 { wrong.append("\(cell.name)_twod") }
        }
        metrics.set("plain", plain ? 1 : 0)
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    /// The colour's bytes in memory order: Metal's nearest codes, sRGB channels encoded, packed as the format stores them
    static func codes(_ cell: Cell) -> [UInt8] {
        let srgb = cell.format == .bgra8Unorm_srgb || cell.format == .rgba8Unorm_srgb
        func code(_ v: Double, _ top: Double, _ encode: Bool) -> UInt32 {
            let e = encode ? (v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1 / 2.4) - 0.055) : v
            return UInt32(min(max(e, 0), 1) * top + 0.5)
        }
        func le(_ word: UInt32, _ bytes: Int) -> [UInt8] {
            (0..<bytes).map { UInt8(truncatingIfNeeded: word >> (8 * $0)) }
        }
        let r = code(colour.red, 255, srgb), g = code(colour.green, 255, srgb), b = code(colour.blue, 255, srgb)
        let a = code(colour.alpha, 255, false)
        switch cell.format {
        case .rgba8Unorm, .rgba8Unorm_srgb: return le(a << 24 | b << 16 | g << 8 | r, 4)
        case .rgb10a2Unorm:
            return le(
                code(colour.alpha, 3, false) << 30 | code(colour.blue, 1023, false) << 20
                    | code(colour.green, 1023, false) << 10 | code(colour.red, 1023, false), 4)
        case .r8Unorm: return [UInt8(r)]
        case .r16Unorm: return le(code(colour.red, 65535, false), 2)
        default: return le(a << 24 | r << 16 | g << 8 | b, 4)
        }
    }
}
