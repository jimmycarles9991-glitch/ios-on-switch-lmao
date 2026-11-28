import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The storage form's read of a 16-bit packed target (m2v fetch/packed.rs), as an M4 Max reads it (host_gpu_tests
/// PackedFetchReadTests): every texel of B5G6R5Unorm and BGR5A1Unorm fetched as float4 and as half4, each channel the
/// nearest half, a missing alpha 1, drawn inside the pixel interlock. `<arm>_off`: texels unlike the model;
/// `<arm>_first`: the first, as texel:read_want; `<arm>_interlocked`: the fetch counter's rise (0: the state had no
/// storage form); path.
struct PackedFetchReadTest: HWTest {
    let id = "gpu.gm20b.packed_fetch_read"
    let name = "A 16-bit packed target's texel reads through the storage form as Metal reads it"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var wrong: [String] = []
        for format in PackedFetchRead.formats {
            for half in [false, true] {
                let arm = "\(format.name)_\(half ? "half" : "float")"
                let before = GM20BFacade.counters(of: gpu.device)["fetch_interlocked"] ?? 0
                let r: PackedFetchRead.Reading
                do {
                    r = try await PackedFetchRead.run(gpu, format, half: half)
                } catch {
                    metrics.set("\(arm)_error", String(describing: error).prefix(80).description)
                    wrong.append("\(arm)_error")
                    continue
                }
                let interlocked = (GM20BFacade.counters(of: gpu.device)["fetch_interlocked"] ?? 0) - before
                metrics.set("\(arm)_off", r.offHalf)
                metrics.set("\(arm)_first", r.first)
                metrics.set("\(arm)_interlocked", interlocked)
                metrics.set("\(arm)_path", r.path)
                if r.offHalf != 0 { wrong.append("\(arm)_off") }
                if interlocked == 0 { wrong.append("\(arm)_interlocked") }
                if r.path != "gm20b" { wrong.append("\(arm)_path") }
            }
        }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
