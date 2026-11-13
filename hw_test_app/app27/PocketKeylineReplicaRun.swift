import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// ui.pocket_keyline's offscreen half: the keyline's state (KeylineReplica) on the process's default device with its
/// other two attachments private (replica_) and memoryless (replica_ml_), and on the m2v device the facade wraps
/// (replica_m2v_). Each run gives <run>_path= and <run>_form= (storage, barrier.<fallback>, split, whole), <run>_wrong=
/// (the first texel off, "-" for none) and <run>_wrong_texels=; the private run also R,G,B,A as drawn and as expected at
/// each mark (<run>_<mark>_rgba, <run>_<mark>_want) and the rows either side of the keyline's row
/// (<run>_above_rgba, <run>_below_rgba), and the other attachments there (<run>_others: RGBA16Float/R32Float).
enum PocketKeylineReplicaRun {
    /// What the replica drew wrong on the default device ("" for nothing), nil when it did not run
    static func run(_ metrics: inout Metrics) async -> String? {
        let gpu: GPU
        do {
            gpu = try GPU.make()
        } catch {
            metrics.set("replica", "no_gpu")
            return nil
        }
        var wrong: String?
        for (prefix, memoryless) in [("replica", false), ("replica_ml", true)] {
            guard let found = await one(gpu, memoryless: memoryless, prefix: prefix, &metrics) else { continue }
            if wrong?.isEmpty ?? true { wrong = found }
        }
        if let m2v = RawFacade.m2vDevice(of: gpu.device), let queue = m2v.makeCommandQueue() {
            let reference = GPU(device: m2v, queue: queue, library: gpu.library)
            _ = await one(reference, memoryless: false, prefix: "replica_m2v", &metrics)
        }
        return wrong
    }

    static func one(_ gpu: GPU, memoryless: Bool, prefix: String, _ metrics: inout Metrics) async -> String? {
        let drawn: KeylineReplica.Drawn
        do {
            drawn = try await KeylineReplica.draw(gpu: gpu, memoryless: memoryless)
        } catch {
            metrics.set(prefix, String(describing: error).replacingOccurrences(of: " ", with: "_"))
            return nil
        }
        let (wrong, texels) = KeylineReplica.judge(drawn)
        metrics.set(prefix + "_path", drawn.path)
        metrics.set(prefix + "_form", drawn.form)
        metrics.set(prefix + "_wrong", wrong.isEmpty ? "-" : wrong)
        metrics.set(prefix + "_wrong_texels", texels)
        guard !memoryless else { return wrong }
        let text = { (values: [Int]) in values.map(String.init).joined(separator: ",") }
        for mark in KeylineReplica.marks {
            let (x, y) = mark.probe
            metrics.set("\(prefix)_\(mark.name)_rgba", text(KeylineReplica.rgba(drawn, x, y)))
            metrics.set("\(prefix)_\(mark.name)_want", text(KeylineReplica.expected(x, y)))
        }
        let (x, y) = KeylineReplica.marks[0].probe
        metrics.set(prefix + "_above_rgba", text(KeylineReplica.rgba(drawn, x, y - 1)))
        metrics.set(prefix + "_below_rgba", text(KeylineReplica.rgba(drawn, x, y + 1)))
        metrics.set(prefix + "_others", KeylineReplica.others(drawn, x, y))
        return wrong
    }
}
