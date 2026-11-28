import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// A sampler descriptor the device has no form of is nil, as Metal answers a sampler it cannot make (it was an m2v twin
/// whose draws went to m2v): a cubic filter with a mip filter or anisotropy, and a NaN lod clamp; a plain sampler and a
/// cubic one with no mip filter are made.
struct SamplerUnsupportedTest: HWTest {
    let id = "gpu.gm20b.sampler_unsupported"
    let name = "Samplers the device has no form of are nil, the rest are made"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let cubic = MTLSamplerMinMagFilter(rawValue: 2) ?? .linear
        func made(_ edit: (MTLSamplerDescriptor) -> Void) -> Bool {
            let d = MTLSamplerDescriptor()
            edit(d)
            return gpu.device.makeSamplerState(descriptor: d) != nil
        }
        let cases: [(String, Bool, (MTLSamplerDescriptor) -> Void)] = [
            ("plain", true, { _ in }),
            (
                "cubic", true,
                {
                    $0.minFilter = cubic; $0.magFilter = cubic
                }
            ),
            (
                "cubic_mip", false,
                {
                    $0.minFilter = cubic; $0.magFilter = cubic; $0.mipFilter = .linear
                }
            ),
            (
                "cubic_anisotropic", false,
                {
                    $0.minFilter = cubic; $0.magFilter = cubic; $0.maxAnisotropy = 4
                }
            ),
            ("nan_lod", false, { $0.lodMinClamp = .nan }),
        ]
        var metrics = Metrics()
        var failures: [String] = []
        for (label, want, edit) in cases {
            let got = made(edit)
            metrics.set(label, got ? "made" : "nil")
            if got != want { failures.append("\(label) \(got ? "made" : "nil")") }
        }
        return failures.isEmpty ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics)
    }
}
