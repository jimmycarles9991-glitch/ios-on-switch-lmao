import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Shader buffer loads with no bounds checks (compile policy), in the app's own connection. in_range: 256 words read
/// through a bound buffer and through an argument buffer's pointer match the buffer (unchecked_in_range_mismatches).
/// unbound: 256 words (offsets 0 to 1020) of a slot the test leaves unbound read zero and the command buffer completes
/// (unchecked_unbound_nonzero_words, unchecked_unbound_status). fault, last: the argument buffer's pointer is 0x800,
/// an address nothing maps; the command buffer fails and the test goes on (unchecked_fault_status error,
/// unchecked_fault_va 0x800 where its error names that address, else "-", unchecked_fault_error its text), and the
/// in_range read right after succeeds (unchecked_after_fault_ok 1); the fault's is the process's one failed command
/// buffer, with no predicate (error:none, failed_cbs=); reopens: GPU channels the facade opened again. The kernel's
/// "channel killed" line is not the app's to check, nor the compositor's presents meanwhile, which the app cannot
/// observe. The fault arm is undefined on Apple's GPUs and runs only on the driver, as the whole test does.
struct UncheckedBufferLoadsTest: HWTest {
    let id = "gpu.gm20b.unchecked_buffer_loads"
    let name = "Buffer loads without bounds checks: in range, an unbound slot, and a pointer nothing maps"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 45 }

    static let words = 256, unmapped: UInt64 = 0x800

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let before = GM20BFacade.counters(of: gpu.device)
        do {
            let (path, mismatches) = try await Self.inRange(gpu, pointer: nil)
            metrics.set("unchecked_in_range_mismatches", mismatches)
            checks.append(("in range: every word read both ways", !path.hasPrefix("error:") && mismatches == 0))
            let (status, nonzero) = try await Self.unbound(gpu)
            metrics.set("unchecked_unbound_nonzero_words", nonzero)
            metrics.set("unchecked_unbound_status", status)
            checks.append(("unbound: completes reading zero", status == "completed" && nonzero == 0))
            let (fault, _) = try await Self.inRange(gpu, pointer: Self.unmapped)
            let error = fault.hasPrefix("error:") ? String(fault.dropFirst("error:".count)) : ""
            metrics.set("unchecked_fault_status", error.isEmpty ? "completed" : "error")
            let named = error.lowercased().range(of: "0x0*800\\b", options: .regularExpression) != nil
            metrics.set("unchecked_fault_va", named ? "0x800" : "-")
            metrics.set("unchecked_fault_error", error.isEmpty ? "-" : error)
            let (after, afterMismatches) = try await Self.inRange(gpu, pointer: nil)
            let ok = !after.hasPrefix("error:") && afterMismatches == 0
            metrics.set("unchecked_after_fault_ok", ok)
            checks.append(("fault: the command buffer fails", !error.isEmpty))
            checks.append(("fault: the next read succeeds", ok))
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        let after = GM20BFacade.counters(of: gpu.device)
        metrics.set("reopens", (after["reopens"] ?? 0) - (before["reopens"] ?? 0))
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        checks.append(
            FailedCounts.judged(
                before: before, after: after, expecting: ["none": 1], own: gpu.paths, &metrics
            )
        )
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func data() -> [UInt32] { (0 ..< words).map { UInt32($0) &* 2_654_435_761 | 1 } }

    /// The command buffer's path and the words read off the buffer either way; `pointer` replaces the argument buffer's
    static func inRange(_ gpu: GPU, pointer: UInt64?) async throws -> (String, Int) {
        let source = try gpu.buffer(data())
        let args = try gpu.buffer([pointer ?? source.gpuAddress])
        let out = try gpu.buffer(words * 8)
        let kernel = try gpu.compute("unchecked_reads")
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(source, offset: 0, index: 0)
            encoder.setBuffer(args, offset: 0, index: 1)
            encoder.setBuffer(out, offset: 0, index: 2)
            encoder.useResource(source, usage: .read)
            encoder.dispatchThreads(
                MTLSize(width: words, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        let got = out.contents().bindMemory(to: UInt32.self, capacity: words * 2)
        let want = data()
        return (timing.path, (0 ..< words).filter { got[2 * $0] != want[$0] || got[2 * $0 + 1] != want[$0] }.count)
    }

    static func unbound(_ gpu: GPU) async throws -> (String, Int) {
        let out = try gpu.buffer([UInt32](repeating: 0xdead_beef, count: words))
        let kernel = try gpu.compute("unchecked_unbound")
        let timing = try await gpu.run(tolerateError: true) { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(kernel)
            encoder.setBuffer(out, offset: 0, index: 2)
            encoder.dispatchThreads(
                MTLSize(width: words, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        let got = out.contents().bindMemory(to: UInt32.self, capacity: words)
        return (timing.path.hasPrefix("error:") ? "error" : "completed", (0 ..< words).filter { got[$0] != 0 }.count)
    }
}
