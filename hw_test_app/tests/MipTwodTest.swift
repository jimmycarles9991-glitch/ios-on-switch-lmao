import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rule mip.twod (drivers/TegraGM20BMetal/raw/fx/fx_mip_route.h): generateMipmaps on the 2D engine, step by step against
/// Metal's definition computed from the level above as the GPU left it (MipReference), for BGRA8, RGBA8, RGBA16Float, R8,
/// RG8 and both 8-bit sRGB formats. The line opens with what decides it: the facade's counters over the run
/// (mip_twod_levels, mip_twod_unproven_levels, mip_3d_levels), the sRGB formats' evidence (`srgb=<fmt>:2d=..:3d=..:l1=..:
/// codes=..`, each <fields beyond one>/<largest difference>: the 2D engine's and the 3D arm's proven steps against Metal's,
/// level 1 of both against each other, and the 2D engine's against the step of the codes taken as unorm; `srgb_first=
/// <fmt>@<2D>|<3D>`, MipSrgbEvidence.swift), `rounding` (how 8-bit 2:1 steps round: MipRounding, round_q*), and one cell a
/// format, `cells=<fmt>:<proven>/<odd>/<3d>` each exact, near (within one code) or wrong, and
/// `gpu_ms=<fmt>:<2D>/<3D>` (a 1024x1024 chain each way). A unorm or sRGB format wrong at a proven step fails it; the odd
/// steps (776x24's 97 -> 48, 255x129's 255 -> 127 and 129 -> 64) and RGBA16Float, whose half ulps the probes did not bound,
/// are reported. The 3D arm draws each level into a one-level texture (paths_3d). Details follow: <fmt>_* (proven),
/// <fmt>_odd_*, <fmt>_3d_*, <fmt>_l1_* (level 1 of both routes compared).
struct MipTwodTest: HWTest {
    let id = "gpu.raw.mip_twod"
    let name = "generateMipmaps on the 2D engine within one code of Metal's step, and its GPU time against 3D passes"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 240 }

    struct Format {
        let name: String
        let pixel: MTLPixelFormat
        let layout: MipLayout
    }

    static let twod = [
        Format(name: "bgra8", pixel: .bgra8Unorm, layout: .unorm([8, 8, 8, 8])),
        Format(name: "rgba8", pixel: .rgba8Unorm, layout: .unorm([8, 8, 8, 8])),
        Format(name: "rgba16f", pixel: .rgba16Float, layout: .floats([16, 16, 16, 16])),
        Format(name: "r8", pixel: .r8Unorm, layout: .unorm([8])),
        Format(name: "rg8", pixel: .rg8Unorm, layout: .unorm([8, 8])),
        Format(name: "bgra8srgb", pixel: .bgra8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
        Format(name: "rgba8srgb", pixel: .rgba8Unorm_srgb, layout: .unorm([8, 8, 8, 8], srgb: true)),
    ]

    static let sizes = [(512, 512), (776, 24), (255, 129)]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard RawFacade.kind(of: gpu.device) == "raw" else {
            return .skip("the default device is not the raw facade (\(RawFacade.kind(of: gpu.device)))")
        }
        var detail = Metrics()
        var wrong: [String] = []
        var paths: Set<String> = [], paths3d: Set<String> = []
        var cells: [String] = [], times: [String] = [], srgb: [String] = [], firsts: [String] = []
        let before = RawFacade.counters(of: gpu.device)
        let rounding = MipRounding()
        for f in Self.twod {
            let chains = MipChains(gpu: gpu, format: f)
            var proven = MipTally(), odd = MipTally(), control = MipTally(), level1 = MipTally()
            var arm3d = "-"
            var evidence = MipSrgbEvidence()
            let isSrgb = f.layout.fields.contains { $0.kind == .srgb }
            for (w, h) in Self.sizes {
                let (twod, path) = try await chains.generated(w, h)
                paths.insert(path)
                chains.compare(twod, w, h, proven: &proven, odd: &odd, rounding: rounding)
                if isSrgb {
                    chains.compareCodes(twod, w, h, into: &evidence.codes)
                    evidence.first2d = evidence.first2d ?? chains.firstBeyondOne(twod, w, h)
                }
                do {
                    let (drawn, path3d) = try await chains.drawn(twod[0], w, h)
                    paths3d.insert(path3d)
                    var unused = MipTally()
                    chains.compare(drawn, w, h, proven: &control, odd: &unused, rounding: nil)
                    level1.add(twod[1], drawn[1], texels: max(1, w / 2) * max(1, h / 2), f.layout)
                    arm3d = control.verdict
                    if isSrgb { evidence.first3d = evidence.first3d ?? chains.firstBeyondOne(drawn, w, h) }
                } catch {
                    paths3d.insert("error")
                    arm3d = "error"
                }
            }
            cells.append("\(f.name):\(proven.verdict)/\(odd.verdict)/\(arm3d)")
            if isSrgb {
                (evidence.twod, evidence.threeD, evidence.level1) = (proven, control, level1)
                srgb.append(evidence.cell(f.name))
                firsts.append("\(f.name)@\(evidence.first2d ?? "-")|\(evidence.first3d ?? "-")")
            }
            Self.report(&detail, f.name, proven)
            Self.report(&detail, f.name + "_odd", odd)
            Self.report(&detail, f.name + "_3d", control)
            Self.report(&detail, f.name + "_l1", level1)
            if proven.wrong > 0 && !f.layout.fields.contains(where: { $0.kind == .float }) {
                wrong.append("\(f.name)_wrong")
            }
            if let (twodMs, drawnMs) = try? await chains.times(1024) {
                times.append(String(format: "%@:%.3f/%.3f", f.name, twodMs, drawnMs))
            } else {
                times.append("\(f.name):error")
            }
        }
        let after = RawFacade.counters(of: gpu.device)
        var metrics = Metrics()
        for key in ["mip_twod_levels", "mip_twod_unproven_levels", "mip_3d_levels"] {
            metrics.set(key, (after[key] ?? 0) - (before[key] ?? 0))
        }
        metrics.set("srgb", srgb.joined(separator: ","))
        metrics.set("srgb_first", firsts.joined(separator: ","))
        metrics.set("rounding", rounding.rule)
        metrics.set("cells", cells.joined(separator: ","))
        metrics.set("gpu_ms", times.joined(separator: ","))
        metrics.set("paths", paths.sorted().joined(separator: ","))
        metrics.set("paths_3d", paths3d.sorted().joined(separator: ","))
        metrics.set("round_q1_low", rounding.low[0])
        metrics.set("round_q2_low", rounding.low[1])
        metrics.set("round_q3_low", rounding.low[2])
        metrics.set("round_q1", rounding.seen[0])
        metrics.set("round_q2", rounding.seen[1])
        metrics.set("round_q3", rounding.seen[2])
        metrics.merge(detail)
        if !after.isEmpty && (after["mip_twod_levels"] ?? 0) == (before["mip_twod_levels"] ?? 0) {
            wrong.insert("no_twod_levels", at: 0)
        }
        if paths != ["raw"] { wrong.insert("paths", at: 0) }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    static func report(_ metrics: inout Metrics, _ prefix: String, _ t: MipTally) {
        metrics.set("\(prefix)_exact", t.exact)
        metrics.set("\(prefix)_within_one", t.withinOne)
        metrics.set("\(prefix)_wrong", t.wrong)
        metrics.set("\(prefix)_max_diff", Int(t.maxDiff))
    }
}
