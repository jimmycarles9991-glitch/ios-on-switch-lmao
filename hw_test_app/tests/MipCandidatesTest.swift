import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The probe cells of rule mip.twod (drivers/TegraGM20BMetal/raw/fx/fx_mip_probe.h), debug builds: the formats the pack's
/// mip states make, every step recorded on the 2D engine (switchRawProbeMipCandidates:) against MipReference, and level 1
/// against the same chain by the shipping route (the 2D engine at the proven steps of fx::twodChainFormat's formats, the
/// format's 3D passes at the others). `candidates=<fmt>:<proven steps>/<odd steps>/<level 1 against the route>`, each
/// exact, near (within one code) or wrong: a format near or exact at the proven steps chains on the 2D engine there. It
/// reports; it fails only when the work left the raw path.
struct MipCandidatesTest: HWTest {
    let id = "gpu.raw.mip_twod_candidates"
    let name =
        "generateMipmaps of the 3D engine's chain formats on the 2D engine, against Metal's step and the 3D route"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 240 }

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
        for f in MipFormats.candidates {
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
