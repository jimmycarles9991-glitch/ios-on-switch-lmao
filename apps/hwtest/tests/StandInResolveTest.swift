import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Multisampled targets of the formats the driver renders through a stand-in (11, 41, 42, 44, 93, 553, 555, 84, 85)
/// at 2x and 4x: a 64x64 target, one draw whose fragment gives each sample its own value (standin_sample_values; the
/// values harness/standinresolveoracle.mm draws, std::mt19937(format * 131 + samples), 84 and 85 through a half as
/// resolvecodesoracle.mm draws them), StoreAndMultisampleResolve into a one-sample texture of the format. The resolve
/// against an M4 Max's bytes of this scene (StandInResolveGoldens.swift, made by StandInResolveCases on an M4 Max) at
/// 256 texels spread over every row (StandInResolveCases.compared): every field within one code, RGB9E5 within one
/// mantissa step of Apple's exponent (the stand-in route on an M4 Max, standinresolveoracle.mm: within one code
/// everywhere; exact 4048/4046 of 4096 texels for 41 at 2x/4x, 4069/4054 for 42, 4069/4058 for 44, 3112/3094 for 553,
/// 3326/3301 for 555, all for 11; 93's rounded mean 4028/3642). Each stored sample read through texture2d_ms must read
/// as a one-sample draw of its value reads, tolerance 0 (an M4 Max: every sample of every case). Per case
/// f<format>_x<n>_off texels not Apple's, _far past one code, _max the largest difference, _samples_off samples off;
/// far_total, samples_off_total; every command buffer raw, failed_cbs=0.
struct StandInResolveTest: HWTest {
    let id = "gpu.gm20b.standin_ms_resolve"
    let name =
        "Multisampled stand-in format targets keep each sample's code and resolve within one code of Apple's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        var far = 0
        var samplesOff = 0
        for format in StandInResolveCases.formats {
            for samples in StandInResolveCases.counts {
                let key = "\(format.key)_x\(samples)"
                guard let text = StandInResolveCases.apple[key],
                      let apple = Data(base64Encoded: text, options: .ignoreUnknownCharacters)
                else {
                    checks.append(("\(key): an M4 Max's resolve", false))
                    continue
                }
                do {
                    let drawn = try await StandInResolveCases.resolved(gpu, format, samples: samples)
                    let judged = StandInResolveCases.judged(format, drawn.resolved, apple: [UInt8](apple))
                    metrics.set(key + "_off", judged.off)
                    metrics.set(key + "_far", judged.far)
                    metrics.set(key + "_max", judged.worst)
                    far += judged.far
                    checks.append(("\(key): every texel within one code of an M4 Max's resolve", judged.far == 0))
                    do {
                        let off = try await StandInResolveCases.samplesOff(gpu, format, drawn)
                        metrics.set(key + "_samples_off", off)
                        samplesOff += off
                        checks.append(("\(key): every sample stores its one-sample code", off == 0))
                    } catch {
                        checks.append(("\(key) samples: \(GPU.describe(error))", false))
                    }
                } catch {
                    checks.append(("\(key): \(GPU.describe(error))", false))
                }
            }
        }
        metrics.set("far_total", far)
        metrics.set("samples_off_total", samplesOff)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        metrics.merge(PathTally.counterMetrics(before: before, after: counted))
        checks.append(("every command buffer on the driver", gpu.paths.offRaw.isEmpty))
        checks.append(FailedCounts.judged(before: before, after: counted, own: gpu.paths, &metrics))
        metrics.fitLog(within: 720, keeping: ["far_total", "samples_off_total", "paths", "failed_cbs"])
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
