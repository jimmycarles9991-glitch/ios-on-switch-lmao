import Foundation
import HWTestCore
import HWTestGPU
import HWTestX1
import Metal

extension X1TwodProbes {
    /// gpu.x1.twod_float_convert (engine-offload survey M1: VideoToolbox's RGhA -> BGRA transfers): the 2D engine
    /// scaling an RGBA16Float source of halves in [0, 1) into BGRA8Unorm at 1:1 and 2:1, point and bilinear, each against
    /// the CPU's filter of the source's halves rounded to 8-bit codes, under Metal's reading and under the reading that
    /// fits best: rf16_to_bgra8=<cell>:<Metal's tally>:<best reading>:<its tally>. Reported, not judged. Apart from
    /// gpu.x1.twod_scale: a float solid colour into a non-float target hung GR twice (BENCH3 section 9).
    static let floatConvert = X1Probe(
        id: "gpu.x1.twod_float_convert", name: "2D-engine RF16x4 to A8R8G8B8 scaled copies at 1:1 and 2:1, reported",
        probe: "copy.twod.f16", risky: true, timeout: 120
    ) { gpu in
        let (cells, paths) = try await convertedCells(gpu)
        var metrics = Metrics()
        metrics.set("rf16_to_bgra8", cells)
        return ProbeFinding.counted([:], paths: paths, metrics: metrics)
    }

    static func convertedCells(_ gpu: GPU) async throws -> (String, [String]) {
        let journal = ProbeJournal(probe: "gpu.x1.twod_float_convert")
        let (half, bgra) = (X1Format.named(["rgba16f"])[0], X1Format.named(["bgra8"])[0])
        var cells: [String] = [], paths: [String] = []
        for (ratio, from, to) in [("1to1", 64, 64), ("2to1", 128, 64)] {
            let (src, codes) = try await source(gpu, half, from, from)
            for point in [true, false] {
                let name = "\(ratio)_\(point ? "point" : "linear")"
                journal.starting(name)
                let written = try gpu.texture(bgra.pixelFormat, to, to, usage: [.renderTarget, .shaderRead])
                let du = (UInt64(from) << 32) / UInt64(to)
                let batch = try await gpu.run(forms: "") { commands in
                    let blit = commands.makeBlitCommandEncoder()!
                    try GM20BProbe.engine(
                        commands,
                        [
                            "kind": "scale", "read": src, "written": written, "du": du, "dv": du, "sx": du / 2,
                            "sy": du / 2, "bilinear": point ? 0 : 1, "corner": 1,
                        ]
                    )
                    blit.endEncoding()
                }
                paths.append(batch.path)
                guard batch.path == "gm20b" else {
                    cells.append("\(name):\(batch.path)")
                    continue
                }
                let bytes = try await gpu.read(written, bytesPerPixel: bgra.bytesPerTexel)
                let filters = Filter.all(point: point, srgb: false)
                var tallies = [X1RuleTally](repeating: X1RuleTally(), count: filters.count)
                for t in 0 ..< to * to {
                    let px = Double(UInt64(t % to) * du + du / 2) / 4_294_967_296
                    let py = Double(UInt64(t / to) * du + du / 2) / 4_294_967_296
                    let got = bgra.codes(bytes, t)
                    for (k, f) in filters.enumerated() {
                        let exact = filtered(codes, from, from, half, x: px, y: py, point: point, f)
                        tallies[k].add(got, exact: exact, channels: bgra.channels, metal: bgra.metalRounding, at: t)
                    }
                }
                let metal = filters.firstIndex(of: .metal(srgb: false)) ?? 0
                let best = tallies.indices.min { tallies[$0].fewestMisses < tallies[$1].fewestMisses } ?? metal
                cells.append("\(name):\(tallies[metal].text):\(filters[best].name):\(tallies[best].text)")
            }
        }
        return (cells.joined(separator: ","), paths)
    }
}
