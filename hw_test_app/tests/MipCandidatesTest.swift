import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The probe cells of rule mip.twod (drivers/TegraGM20BMetal/raw/fx/fx_mip_probe.h), debug builds: the formats the pack's
/// mip states make, recorded on the 2D engine (switchRawProbeMipCandidates:), each step against MipReference and level 1
/// against the same chain by the format's 3D passes. `candidates=<fmt>:<proven steps>/<odd steps>/<level 1 against 3D>`,
/// each exact, near (within one code) or wrong: a format near or exact at the proven steps can join fx::twodFormat. It
/// reports; it fails only when the work left the raw path.
struct MipCandidatesTest: HWTest {
    let id = "gpu.raw.mip_twod_candidates"
    let name =
        "generateMipmaps of the 3D engine's chain formats on the 2D engine, against Metal's step and the 3D route"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 240 }

    static let candidates = [
        MipTwodTest.Format(name: "a8", pixel: .a8Unorm, layout: .unorm([8])),
        MipTwodTest.Format(name: "r16", pixel: .r16Unorm, layout: .unorm([16])),
        MipTwodTest.Format(name: "r16f", pixel: .r16Float, layout: .floats([16])),
        MipTwodTest.Format(name: "b5g6r5", pixel: .b5g6r5Unorm, layout: .unorm([5, 6, 5])),
        MipTwodTest.Format(name: "bgr5a1", pixel: .bgr5A1Unorm, layout: .unorm([1, 5, 5, 5])),
        MipTwodTest.Format(name: "r32f", pixel: .r32Float, layout: .floats([32])),
        MipTwodTest.Format(name: "rg16", pixel: .rg16Unorm, layout: .unorm([16, 16])),
        MipTwodTest.Format(name: "rg16f", pixel: .rg16Float, layout: .floats([16, 16])),
        MipTwodTest.Format(name: "rgb10a2", pixel: .rgb10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        MipTwodTest.Format(name: "rg11b10f", pixel: .rg11b10Float, layout: .floats([11, 11, 10])),
        MipTwodTest.Format(name: "bgr10a2", pixel: .bgr10a2Unorm, layout: .unorm([10, 10, 10, 2])),
        MipTwodTest.Format(name: "rg32f", pixel: .rg32Float, layout: .floats([32, 32])),
        MipTwodTest.Format(name: "rgba16", pixel: .rgba16Unorm, layout: .unorm([16, 16, 16, 16])),
        MipTwodTest.Format(name: "rgba32f", pixel: .rgba32Float, layout: .floats([32, 32, 32, 32])),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        guard MipChains.probe(gpu.device, false) != nil else { return .skip("the facade has no mip probe switch") }
        defer { _ = MipChains.probe(gpu.device, false) }
        var detail = Metrics()
        var cells: [String] = []
        var paths: Set<String> = []
        let before = RawFacade.counters(of: gpu.device)
        for f in Self.candidates {
            let chains = MipChains(gpu: gpu, format: f)
            var proven = MipTally(), odd = MipTally(), level1 = MipTally()
            do {
                for (w, h) in MipTwodTest.sizes {
                    _ = MipChains.probe(gpu.device, false)
                    let (passes, _) = try await chains.generated(w, h)
                    _ = MipChains.probe(gpu.device, true)
                    let (twod, path) = try await chains.generated(w, h)
                    paths.insert(path)
                    chains.compare(twod, w, h, proven: &proven, odd: &odd, rounding: nil)
                    level1.add(twod[1], passes[1], texels: twod[1].count / f.layout.bytes, f.layout)
                }
            } catch {
                cells.append("\(f.name):error")
                continue
            }
            cells.append("\(f.name):\(proven.verdict)/\(odd.verdict)/\(level1.verdict)")
            MipTwodTest.report(&detail, "cand_" + f.name, proven)
            MipTwodTest.report(&detail, "cand_\(f.name)_odd", odd)
            MipTwodTest.report(&detail, "cand_\(f.name)_l1", level1)
        }
        let after = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        metrics.set("candidates", cells.joined(separator: ","))
        metrics.set("mip_twod_levels", (after["mip_twod_levels"] ?? 0) - (before["mip_twod_levels"] ?? 0))
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.merge(detail)
        return paths == ["raw"] ? .pass(metrics) : .fail("paths", metrics)
    }
}
