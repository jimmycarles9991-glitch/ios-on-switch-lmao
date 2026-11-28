import Foundation
import HWTestCore
import HWTestKit
import SwitchBench

/// MapSparse on a TegraGM20B connection of the test's own (bench/bench_sparse.cpp), every read and write by the copy
/// engine, one job each, waited for. A GM20B PTE with VALID 0 and VOL 1 reads 0 and drops writes for every client with
/// no fault (nouveau vmmgm200.c gm200_vmm_pgt_sparse, nvgpu mm_gk20a.c __update_pte_sparse), which every expected
/// value here is; no Apple GPU has such PTEs to measure. sparse_head_bytes_kept, sparse_tail_read_zero: a copy across
/// a 64 KiB BO's end into 4 KiB sparse PTEs; sparse_big_read_zero: two 128 KiB sparse PTEs; sparse_beside_big_read_zero:
/// a 4 KiB sparse PTE in the page directory entry of a contiguous 128 KiB BO's big page (nvgpu never mixes the two in
/// one entry); sparse_write_dropped: a fill into sparse PTEs reads back 0 and no byte of the BOs changed;
/// sparse_replaces_map, map_replaces_sparse: each bind replaces the other; sparse_bad_args_refused: a VA off a page and
/// one ending past va_end are kIOReturnBadArgument; sparse_faults: no channel error through all of it. Last, on a
/// throwaway channel, a read of the range unmapped kills that channel with an MMU fault inside it
/// (unmapped_read_faults; the kernel logs it as the deliberate "fault at" line of a binding none).
struct KernelSparseMapTest: HWTest {
    let id = "gpu.kernel.sparse_map"
    let name = "Sparse GPU VA reads zero and drops writes at 4 KiB and 128 KiB, beside a big page, until an unmap"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    func run(_ context: TestContext) async throws -> Outcome {
        var r = switch_bench_sparse()
        switch_bench_sparse_map(&r)
        let readings: [(String, Int32, Int32)] = [
            ("sparse_info_bit", r.info_bit, 1),
            ("sparse_head_bytes_kept", r.head_bytes_kept, 1),
            ("sparse_tail_read_zero", r.tail_read_zero, 1),
            ("sparse_big_read_zero", r.big_read_zero, 1),
            ("sparse_beside_big_read_zero", r.beside_big_read_zero, 1),
            ("sparse_write_dropped", r.write_dropped, 1),
            ("sparse_replaces_map", r.replaces_map, 1),
            ("map_replaces_sparse", r.map_replaces, 1),
            ("sparse_bad_args_refused", r.bad_args_refused, 1),
            ("sparse_faults", r.faults, 0),
            ("unmapped_read_faults", r.unmapped_read_faults, 1),
        ]
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        for (key, value, want) in readings {
            if value >= 0 { metrics.set(key, Int(value)) }
            checks.append(("\(key)=\(value < 0 ? "unreached" : String(value)) not \(want)", value == want))
        }
        metrics.set("bad_args_kr", [r.bad_args_kr.0, r.bad_args_kr.1].map(Self.hex).joined(separator: ","))
        metrics.set("fault_error", Int(r.fault_error))
        metrics.set("fault_va", "0x" + String(r.fault_va, radix: 16))
        metrics.set("unmapped_va", "0x" + String(r.unmapped_va, radix: 16))
        let why = withUnsafeBytes(of: &r.why) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
        if !why.isEmpty { metrics.set("stopped", why) }
        return .judged(checks, metrics, detail: why)
    }

    static func hex(_ kr: Int32) -> String { "0x" + String(UInt32(bitPattern: kr), radix: 16) }
}
