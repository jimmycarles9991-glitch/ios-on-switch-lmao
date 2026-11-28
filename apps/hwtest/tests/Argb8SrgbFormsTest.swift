import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// ARGB8Unorm_sRGB (85) against RGBA8Unorm_sRGB (71) under 85's channel rotation, on the driver
/// (gpu/Argb8SrgbForms.swift): every code read by a kernel, sampled linearly, written by a pass and by a kernel.
/// reads_off= linear_off= pass_writes_off= kernel_writes_off= texels that differ, first= the first. An M4 Max's answer
/// is host_gpu_tests Argb8SrgbFormTests'; the driver must give the same, every field 0.
struct Argb8SrgbFormsTest: HWTest {
    let id = "gpu.gm20b.argb8_srgb_forms"
    let name = "ARGB8Unorm_sRGB reads, samples and writes as RGBA8Unorm_sRGB with its channels rotated, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let r = try await Argb8SrgbForms.measure(gpu)
        var metrics = Metrics()
        metrics.set("reads_off", r.readsOff)
        metrics.set("linear_off", r.linearOff)
        metrics.set("pass_writes_off", r.passWritesOff)
        metrics.set("kernel_writes_off", r.kernelWritesOff)
        metrics.set("first", r.first)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let checks = [
            ("a kernel reads every code as RGBA8Unorm_sRGB's rotated (\(r.readsOff) off)", r.readsOff == 0),
            ("a linear sample as RGBA8Unorm_sRGB's rotated (\(r.linearOff) off)", r.linearOff == 0),
            ("a pass writes as RGBA8Unorm_sRGB's rotated (\(r.passWritesOff) off)", r.passWritesOff == 0),
            ("a kernel writes as RGBA8Unorm_sRGB's rotated (\(r.kernelWritesOff) off)", r.kernelWritesOff == 0),
            ("every command buffer on the driver", gpu.paths.offRaw.isEmpty),
        ]
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
