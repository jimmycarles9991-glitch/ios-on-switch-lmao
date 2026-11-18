import Foundation
import HWTestCore
import Metal

/// Metal's three SIMD-group feature rows on the GPU's own SIMD width, each lane's word against the CPU's: permute and
/// votes (supportsSIMDGroup), reduction and prefix (supportsSIMDReduction), shift-and-fill
/// (supportsSIMDShuffleAndFill). On the raw device each row's answer is reported beside its count.
struct SimdGroupScene: GPUScene {
    let id = "gpu.compute.simd_group"
    let name = "SIMD-group permute, reduction and shift-and-fill against the CPU"

    static let threads = 4096
    static let wordsPerThread = 24
    static let rows: [(name: String, answer: String, ops: ClosedRange<Int>)] = [
        ("permute", "supportsSIMDGroup", 1...7),
        ("reduction", "supportsSIMDReduction", 8...18),
        ("fill", "supportsSIMDShuffleAndFill", 19...22),
    ]

    static func input(_ i: Int) -> UInt32 {
        var h = UInt32(truncatingIfNeeded: i &* 2_654_435_761) ^ 0x9E37_79B9
        h ^= h >> 15
        h = h &* 0x2C1B_3C6D
        return h ^ (h >> 12)
    }

    /// The word lane `lane` of a group holding `v` writes for operation `op` (Scenes.metal simd_group)
    static func expected(_ op: Int, _ v: [UInt32], _ lane: Int) -> UInt32 {
        let w = v.count
        let fill = v.map { $0 ^ 0xA5A5_A5A5 }
        let low = v.map { $0 & 0xFFFF }
        let base = lane / 8 * 8, inGroup = lane % 8
        switch op {
        case 1: return v[(lane * 7 + 3) % w]
        case 2: return v[5]
        case 3: return lane >= 3 ? v[lane - 3] : v[lane]
        case 4: return lane + 3 < w ? v[lane + 3] : v[lane]
        case 5: return v[lane ^ 5]
        case 6: return v.indices.reduce(0) { $0 | (v[$1] & 1 != 0 && $1 < 32 ? 1 << UInt32($1) : 0) }
        case 7: return (v.allSatisfy { $0 != 0 } ? 1 : 0) | (v.contains { $0 & 0xFF == 0 } ? 2 : 0)
        case 8: return low.reduce(0, &+)
        case 9: return v.reduce(1) { $0 &* (($1 & 3) + 1) }
        case 10: return v.min()!
        case 11: return v.max()!
        case 12: return v.reduce(~0) { $0 & ($1 | 0x8000_0001) }
        case 13: return v.reduce(0) { $0 | ($1 & 0x1020_4080) }
        case 14: return v.reduce(0, ^)
        case 15: return low[...lane].reduce(0, &+)
        case 16: return low[..<lane].reduce(0, &+)
        case 17: return Float(v.reduce(0) { $0 + ($1 & 0xFF) }).bitPattern
        case 18: return Float(v[..<lane].reduce(0) { $0 + ($1 & 0xFF) }).bitPattern
        case 19: return lane >= 3 ? v[lane - 3] : fill[lane - 3 + w]
        case 20: return lane + 3 < w ? v[lane + 3] : fill[lane + 3 - w]
        case 21: return inGroup >= 3 ? v[lane - 3] : fill[base + inGroup - 3 + 8]
        case 22: return inGroup + 3 < 8 ? v[lane + 3] : fill[base + inGroup + 3 - 8]
        default: return 0
        }
    }

    /// The float sums compare as values: Apple's GPUs start an exclusive prefix sum at -0, which adds as +0 does
    static func same(_ op: Int, _ got: UInt32, _ want: UInt32) -> Bool {
        (17...18).contains(op) ? Float(bitPattern: got) == Float(bitPattern: want) : got == want
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let values = (0..<Self.threads).map(Self.input)
        let out = try gpu.buffer(Self.threads * Self.wordsPerThread * 4)
        let pipeline = try gpu.compute("simd_group")
        let width = pipeline.threadExecutionWidth
        let timing = try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(pipeline)
            encoder.setBuffer(try gpu.buffer(values), offset: 0, index: 0)
            encoder.setBuffer(out, offset: 0, index: 1)
            encoder.dispatchThreads(
                MTLSize(width: Self.threads, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 2 * width, height: 1, depth: 1))
            encoder.endEncoding()
        }
        let words = UnsafeBufferPointer(
            start: out.contents().bindMemory(to: UInt32.self, capacity: Self.threads * Self.wordsPerThread),
            count: Self.threads * Self.wordsPerThread)
        var layoutWrong = 0
        var wrong = [Int](repeating: 0, count: Self.rows.count)
        var first: [String] = []
        for id in 0..<Self.threads {
            let lane = id % width, group = Array(values[(id - lane)..<(id - lane + width)])
            let word = { (op: Int) in words[id * Self.wordsPerThread + op] }
            if word(0) != UInt32(lane) | UInt32(width) << 16 || word(23) != UInt32(id) { layoutWrong += 1 }
            for (row, entry) in Self.rows.enumerated() {
                for op in entry.ops where !Self.same(op, word(op), Self.expected(op, group, lane)) {
                    wrong[row] += 1
                    if first.count < 4 {
                        first.append("op \(op) thread \(id): \(word(op)), want \(Self.expected(op, group, lane))")
                    }
                }
            }
        }
        result.metrics.set("simd_width", width)
        result.metrics.set("path", timing.path)
        result.metrics.set("layout_wrong", layoutWrong)
        for (row, entry) in Self.rows.enumerated() { result.metrics.set(entry.name + "_wrong", wrong[row]) }
        if RawFacade.kind(of: gpu.device) == "raw" {
            result.expect(timing.path == "raw", "SIMD-group operations left raw: \(timing.path)")
            let object = gpu.device as AnyObject as! NSObject
            for entry in Self.rows {
                let answer = (object.value(forKey: entry.answer) as? NSNumber)?.intValue ?? -1
                result.metrics.set(entry.name + "_answer", answer)
                result.expect(answer == 1, "\(entry.answer) answers \(answer)")
            }
        }
        result.expect(layoutWrong == 0, "\(layoutWrong) threads not at the lane and width the dispatch lays out")
        result.expect(first.isEmpty, "\(wrong.reduce(0, +)) wrong: " + first.joined(separator: "; "))
        return result
    }
}
