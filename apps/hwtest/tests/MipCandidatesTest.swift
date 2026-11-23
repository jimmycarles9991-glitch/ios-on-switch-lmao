import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The probe cells of rule mip.twod (drivers/TegraGM20BMetal/fx/fx_mip_probe.h), debug builds: the formats the pack's
/// mip states make, every step recorded on the 2D engine (switchMetalProbeMipCandidates:) against MipReference, and level 1
/// against the same chain by the shipping route (the 2D engine at the proven steps of fx::twodChainFormat's formats, the
/// format's mip state at the others). `candidates=<fmt>:<proven steps>/<odd steps>/<level 1 against the route>`, each
/// exact, near (within one code) or wrong: a format near or exact at the proven steps chains on the 2D engine there.
/// cand_<fmt>_route_* and cand_<fmt>_route_odd_* hold the shipping route's steps against MipReference. It reports; it
/// fails when the work left the driver or when a candidate's command buffer failed or threw (`<fmt>:error`, the
/// formats and the first error in the detail).
struct MipCandidatesTest: HWTest {
    let id = "gpu.gm20b.mip_twod_candidates"
    let name =
        "generateMipmaps of the 3D engine's chain formats on the 2D engine, against Metal's step and the 3D route"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 240 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        guard MipChains.probe(gpu.device, false) != nil else {
            return .skip(GM20BFacade.missing("switchMetalProbeMipCandidates:", on: gpu.device))
        }
        defer { _ = MipChains.probe(gpu.device, false) }
        var detail = Metrics()
        var cells: [String] = []
        var paths: Set<String> = []
        var failed: [String] = []
        var firstError: String?
        let before = GM20BFacade.counters(of: gpu.device)
        for f in MipFormats.candidates {
            let chains = MipChains(gpu: gpu, format: f)
            var proven = MipTally(), odd = MipTally(), level1 = MipTally(), route = MipTally(), routeOdd = MipTally()
            do {
                for (w, h) in MipTwodTest.sizes {
                    _ = MipChains.probe(gpu.device, false)
                    let (passes, _) = try await chains.generated(w, h)
                    chains.compare(passes, w, h, proven: &route, odd: &routeOdd, rounding: nil)
                    _ = MipChains.probe(gpu.device, true)
                    let (twod, path) = try await chains.generated(w, h)
                    paths.insert(path)
                    chains.compare(twod, w, h, proven: &proven, odd: &odd, rounding: nil)
                    level1.add(twod[1], passes[1], texels: twod[1].count / f.layout.bytes, f.layout)
                }
            } catch {
                cells.append("\(f.name):error")
                failed.append(f.name)
                firstError = firstError ?? "\(f.name): \(error)"
                continue
            }
            cells.append("\(f.name):\(proven.verdict)/\(odd.verdict)/\(level1.verdict)")
            MipTwodTest.report(&detail, "cand_" + f.name, proven)
            MipTwodTest.report(&detail, "cand_\(f.name)_odd", odd)
            MipTwodTest.report(&detail, "cand_\(f.name)_l1", level1)
            MipTwodTest.report(&detail, "cand_\(f.name)_route", route)
            MipTwodTest.report(&detail, "cand_\(f.name)_route_odd", routeOdd)
        }
        let after = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        metrics.set("candidates", cells.joined(separator: ","))
        metrics.set("mip_twod_levels", (after["mip_twod_levels"] ?? 0) - (before["mip_twod_levels"] ?? 0))
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.merge(detail)
        var wrong: [String] = []
        if paths != ["gm20b"] { wrong.append("paths") }
        if let firstError {
            wrong.append("failed or threw: \(failed.joined(separator: ",")) (first \(firstError))")
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: "; "), metrics)
    }
}
