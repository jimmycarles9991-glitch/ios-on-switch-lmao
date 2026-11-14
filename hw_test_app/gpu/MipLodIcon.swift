import Foundation
import HWTestCore
import Metal

extension MipLod {
    static let iconSide = 128
    static let iconRadius = 30.0
    public static let iconSides = [29, 40, 60, 76, 100]

    /// The rounded rectangle's coverage of texel (x, y), 8x8 samples
    static func coverage(_ x: Int, _ y: Int) -> Double {
        let s = Double(iconSide), r = iconRadius
        var inside = 0
        for j in 0..<8 {
            for i in 0..<8 {
                let px = Double(x) + (Double(i) + 0.5) / 8, py = Double(y) + (Double(j) + 0.5) / 8
                let dx = max(r - px, px - (s - r), 0), dy = max(r - py, py - (s - r), 0)
                if dx * dx + dy * dy <= r * r { inside += 1 }
            }
        }
        return Double(inside) / 64
    }

    /// Level 0: premultiplied white, every channel the coverage's code
    static func iconLevel0() -> [UInt8] {
        (0..<iconSide * iconSide).flatMap { t -> [UInt8] in
            let c = UInt8((coverage(t % iconSide, t / iconSide) * 255).rounded())
            return [c, c, c, c]
        }
    }

    /// Metal's bilinear sample of a level at normalised u, v: taps at 1/256 (MipReference.tap's law at any coordinate),
    /// clamped to the edge
    static func bilinear(_ level: [UInt8], _ side: Int, _ u: Double, _ v: Double, channel: Int) -> Double {
        func axis(_ c: Double) -> (Int, Int, Double) {
            let p = c * Double(side) - 0.5, f = p.rounded(.down)
            var w = ((p - f) * 256).rounded(.toNearestOrAwayFromZero) / 256
            var i0 = Int(f)
            if w == 1 {
                i0 += 1
                w = 0
            }
            return (min(max(i0, 0), side - 1), min(max(i0 + 1, 0), side - 1), w)
        }
        let (x0, x1, wx) = axis(u), (y0, y1, wy) = axis(v)
        let at = { (x: Int, y: Int) in Double(level[(y * side + x) * 4 + channel]) }
        return (at(x0, y0) * (1 - wx) + at(x1, y0) * wx) * (1 - wy) + (at(x0, y1) * (1 - wx) + at(x1, y1) * wx) * wy
    }

    /// The CPU's reference of Metal's trilinear draw of `levels` (level 0 iconSide wide) at side x side, BGRA8 bytes
    static func iconReference(_ levels: [[UInt8]], side: Int) -> [UInt8] {
        let lod = log2(Double(iconSide) / Double(side))
        let l = min(Int(lod.rounded(.down)), levels.count - 1), f = lod - Double(l)
        let l1 = min(l + 1, levels.count - 1)
        var out = [UInt8](repeating: 0, count: side * side * 4)
        for y in 0..<side {
            for x in 0..<side {
                let u = (Double(x) + 0.5) / Double(side), v = (Double(y) + 0.5) / Double(side)
                for c in 0..<4 {
                    let a = bilinear(levels[l], max(1, iconSide >> l), u, v, channel: c)
                    let b = bilinear(levels[l1], max(1, iconSide >> l1), u, v, channel: c)
                    out[(y * side + x) * 4 + c] = UInt8(min(max(a * (1 - f) + b * f, 0), 255).rounded())
                }
            }
        }
        return out
    }

    /// Each side's draw against the reference: (codes beyond iconTolerance, the largest difference)
    public static func icon(_ gpu: GPU) async throws -> (cells: [(side: Int, beyond: Int, maxDiff: Int)], path: String)
    {
        let level0 = iconLevel0()
        let count = Int(log2(Double(iconSide))) + 1
        let chain = try gpu.texture(
            .bgra8Unorm, iconSide, iconSide, usage: [.shaderRead, .renderTarget], mipmaps: count)
        try await gpu.upload(level0, to: chain, bytesPerRow: iconSide * 4)
        let path = try await gpu.run { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.generateMipmaps(for: chain)
            blit.endEncoding()
        }.path
        let layout = MipLayout.unorm([8, 8, 8, 8])
        var metal = [level0]
        for l in 1..<count {
            let s = max(1, iconSide >> (l - 1)), d = max(1, iconSide >> l)
            metal.append(MipReference.step(metal[l - 1], s, s, d, d, layout))
        }
        let trilinear = try sampler(gpu.device, Arm(name: "icon", side: 0))
        var cells: [(side: Int, beyond: Int, maxDiff: Int)] = []
        for side in iconSides {
            let (got, _) = try await draw(gpu, chain, trilinear, side: side, bias: nil)
            let want = iconReference(metal, side: side)
            var beyond = 0, largest = 0
            for i in 0..<got.count {
                let d = abs(Int(got[i]) - Int(want[i]))
                largest = max(largest, d)
                if d > iconTolerance { beyond += 1 }
            }
            cells.append((side, beyond, largest))
        }
        return (cells, path)
    }
}
