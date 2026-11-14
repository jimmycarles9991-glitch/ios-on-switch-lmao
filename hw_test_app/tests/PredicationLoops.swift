import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// gpu.raw.predication's loops (drivers/TegraGM20BMetal/raw/runtime/raw_loop_copies.h): Whiles that count down a
/// GPU-written word from 0, 1, a data-dependent 37, the copies' bound less one and the bound; a DoWhile that runs
/// once; a While inside a While; then a While one past the bound, which the raw path lays down again from its
/// completion and so must run to its end, and the GPU time a copy after the loop's end costs (one fetch where the
/// fetcher sees the heads the copies write).
struct PredicationLoops {
    typealias StartWhile = @convention(c) (AnyObject, Selector, MTLBuffer, Int, Int, UInt32) -> Void
    typealias EndDoWhile = @convention(c) (AnyObject, Selector, MTLBuffer, Int, Int, UInt32) -> Bool
    typealias Plain = @convention(c) (AnyObject, Selector) -> Void
    typealias End = @convention(c) (AnyObject, Selector) -> Bool
    static let startWhile = NSSelectorFromString("encodeStartWhile:offset:comparison:referenceValue:")
    static let endWhile = NSSelectorFromString("encodeEndWhile")
    static let startDoWhile = NSSelectorFromString("encodeStartDoWhile")
    static let endDoWhile = NSSelectorFromString("encodeEndDoWhile:offset:comparison:referenceValue:")
    static let notEqual = 1

    /// LoopCopies::kOutermostCopies
    static let bound: UInt32 = 1024
    static let counts: [UInt32] = [0, 1, 37, bound - 1, bound]
    static let nestedOuter: UInt32 = 3, nestedInner: UInt32 = 2

    let gpu: GPU
    let store: MTLComputePipelineState, count: MTLComputePipelineState, step: MTLComputePipelineState
    let words: MTLBuffer, marks: MTLBuffer
    var ends: [Bool] = []

    init(_ gpu: GPU) throws {
        self.gpu = gpu
        store = try gpu.compute("predication_store")
        count = try gpu.compute("predication_count")
        step = try gpu.compute("predication_step")
        words = try gpu.buffer(4 * 64)
        marks = try gpu.buffer(4 * 64)
    }

    func one(
        _ e: MTLComputeCommandEncoder, _ state: MTLComputePipelineState, _ buffer: MTLBuffer, _ at: Int,
        _ value: UInt32
    ) {
        var pair = SIMD2<UInt32>(UInt32(at), value)
        e.setComputePipelineState(state)
        e.setBuffer(buffer, offset: 0, index: 0)
        e.setBytes(&pair, length: 8, index: 1)
        e.dispatchThreadgroups(
            MTLSize(width: 1, height: 1, depth: 1),
            threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        e.memoryBarrier(scope: .buffers)
    }

    func startWhile(_ e: MTLComputeCommandEncoder, word: Int) {
        let o = e as AnyObject
        unsafeBitCast(o.method(for: Self.startWhile), to: StartWhile.self)(
            o, Self.startWhile, words, 4 * word, Self.notEqual, 0)
    }

    mutating func endWhile(_ e: MTLComputeCommandEncoder) {
        let o = e as AnyObject
        ends.append(unsafeBitCast(o.method(for: Self.endWhile), to: End.self)(o, Self.endWhile))
    }

    /// while (words[word] != 0) { marks[mark] += 1; words[word] -= 1 }, the word set to `from` by the GPU first
    mutating func countDown(_ e: MTLComputeCommandEncoder, word: Int, mark: Int, from: UInt32) {
        one(e, store, words, word, from)
        startWhile(e, word: word)
        one(e, count, marks, mark, 1)
        one(e, step, words, word, 1)
        endWhile(e)
    }

    mutating func encodeLoops(_ commands: MTLCommandBuffer) {
        let e = commands.makeComputeCommandEncoder()!
        for (i, from) in Self.counts.enumerated() { countDown(e, word: i, mark: i, from: from) }
        let doWord = Self.counts.count, doMark = Self.counts.count
        one(e, store, words, doWord, 0)
        let o = e as AnyObject
        unsafeBitCast(o.method(for: Self.startDoWhile), to: Plain.self)(o, Self.startDoWhile)
        one(e, count, marks, doMark, 1)
        ends.append(
            unsafeBitCast(o.method(for: Self.endDoWhile), to: EndDoWhile.self)(
                o, Self.endDoWhile, words, 4 * doWord, Self.notEqual, 0))
        let outerWord = doWord + 1, innerWord = doWord + 2, nestedMark = doMark + 1
        one(e, store, words, outerWord, Self.nestedOuter)
        startWhile(e, word: outerWord)
        countDown(e, word: innerWord, mark: nestedMark, from: Self.nestedInner)
        one(e, step, words, outerWord, 1)
        endWhile(e)
        e.endEncoding()
    }

    /// The loop counts, the DoWhile's and the nested loop's, and what is wrong with them
    mutating func run(_ metrics: inout Metrics) async throws -> [String] {
        memset(marks.contents(), 0, 4 * 64)
        ends = []
        let ran = try await gpu.run(tolerateError: true) { commands in self.encodeLoops(commands) }
        let m = marks.contents().bindMemory(to: UInt32.self, capacity: 64)
        let counted = Self.counts.indices.map { m[$0] }
        let doOnce = m[Self.counts.count], nested = m[Self.counts.count + 1]
        var wrong: [String] = []
        metrics.set("loop_path", ran.path)
        metrics.set("loop_counts", counted.map(String.init).joined(separator: ","))
        metrics.set("dowhile_runs", Int(doOnce))
        metrics.set("nested_runs", Int(nested))
        metrics.set("loop_ends_yes", "\(ends.filter { $0 }.count)/\(ends.count)")
        metrics.set("loop_gpu_ms", ran.gpuMilliseconds)
        if ran.path != "raw" { wrong.append("loop_path") }
        if counted != Self.counts { wrong.append("loop_counts") }
        if doOnce != 1 { wrong.append("dowhile") }
        if nested != Self.nestedOuter * Self.nestedInner { wrong.append("nested") }
        if ends.contains(false) { wrong.append("loop_ends") }

        memset(marks.contents(), 0, 4 * 64)
        let past = try await gpu.run(tolerateError: true) { commands in
            let e = commands.makeComputeCommandEncoder()!
            self.countDown(e, word: 0, mark: 0, from: Self.bound + 1)
            e.endEncoding()
        }
        let pastRuns = marks.contents().bindMemory(to: UInt32.self, capacity: 1)[0]
        metrics.set("overflow", past.path.hasPrefix("error:") ? "error" : past.path)
        metrics.set("overflow_runs", Int(pastRuns))
        if past.path != "raw" || pastRuns != Self.bound + 1 { wrong.append("overflow") }

        let plain = try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            self.one(e, self.store, self.words, 0, 1)
            self.one(e, self.count, self.marks, 0, 1)
            self.one(e, self.step, self.words, 0, 1)
            e.endEncoding()
        }
        let exitAtOne = try await gpu.run { commands in
            let e = commands.makeComputeCommandEncoder()!
            self.countDown(e, word: 0, mark: 0, from: 1)
            e.endEncoding()
        }
        let tail = (exitAtOne.gpuMilliseconds - plain.gpuMilliseconds) * 1000 / Double(Self.bound - 1)
        metrics.set("tail_us_per_copy", String(format: "%.3f", tail))
        return wrong
    }
}
