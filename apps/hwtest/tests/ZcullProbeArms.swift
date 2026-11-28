import Foundation
import HWTestCore
import HWTestGPU
import HWTestGPUModels
import Metal
import SwitchBench

/// gpu.gm20b.zcull's info and side_effects arms. info: GetInfo carries TegraGM20BInfoZcull (info_zcull_flag 1) and its
/// zcull[12] equals method 'ZC''s reply word for word (info_zcull_matches 1): 2048 aliquots (info_aliquots), 1024 pixel
/// squares an aliquot (info_pixel_squares), as nvgpu's gr_gk20a_get_zcull_info gives GM20B. side_effects: far quads
/// whose function adds to a counter with no early fragment tests (gpu/ZcullSideEffects.swift), behind a near quad, with
/// the pass's zcull region and with it invalidated before every draw: Metal runs the function for every far fragment,
/// 64 x 1280 x 720 = 58,982,400 on an M4 Max (side_effect_count_zcull, side_effect_count_invalidated,
/// side_effect_expected all equal; if not, zcull.h's zcullInvalidates must take a program's device writes as unsafe).
extension ZcullTest {
    func info(_ metrics: inout Metrics, _ wrong: inout [String]) {
        var flags: UInt32 = 0
        var info = [UInt32](repeating: 0, count: 12), zc = [UInt32](repeating: 0, count: 12)
        let kr = info.withUnsafeMutableBufferPointer { i in
            zc.withUnsafeMutableBufferPointer { z in switch_bench_zcull_info(&flags, i.baseAddress, z.baseAddress) }
        }
        metrics.set("info_zcull_flag", Int(flags >> 2 & 1))
        metrics.set("info_zc_kr", "0x" + String(UInt32(bitPattern: kr), radix: 16))
        metrics.set("info_zcull_matches", kr == 0 && info == zc)
        metrics.set("info_aliquots", Int(zc[3]))
        metrics.set("info_pixel_squares", Int(zc[2]))
        if flags >> 2 & 1 != 1 || kr != 0 || info != zc || zc[3] != 2048 || zc[2] != 1024 { wrong.append("info") }
    }

    func sideEffects(
        _ gpu: GPU, _ target: MTLTexture, _ metrics: inout Metrics, _ wrong: inout [String], _ paths: inout Set<String>
    ) async throws {
        let depth = try gpu.texture(
            .depth32Float,
            target.width,
            target.height,
            usage: [.renderTarget],
            storage: .memoryless
        )
        let expected = ZcullSideEffects.farQuads * target.width * target.height
        var counts: [Int] = []
        for invalidated in [false, true] {
            let count = try gpu.buffer([UInt32(0)])
            let pass = try ZcullSideEffects.pass(gpu, target: target, depth: depth, count: count)
            if invalidated {
                paths.insert(try await GM20BMethods.draw(gpu, before: [(0x1958, 0)], after: [], pass).path)
            } else {
                paths.insert(try await gpu.run(pass).path)
            }
            counts.append(Int(count.contents().load(as: UInt32.self)))
        }
        metrics.set("side_effect_count_zcull", counts[0])
        metrics.set("side_effect_count_invalidated", counts[1])
        metrics.set("side_effect_expected", expected)
        if counts != [expected, expected] { wrong.append("side_effects") }
    }
}
