import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Metal's command predication SPI on the raw path (drivers/TegraGM20BMetal/raw/runtime/raw_predication.h): each of the
/// six compares at its edge values, two levels of If/Else, a block spanning two compute encoders of its unit, and the
/// draw, clear and blit after a failed block, all in one command buffer; then the loops (PredicationLoops.swift). Every predicate word is written by a dispatch
/// before its record, so neither the CPU nor the test reads or writes it, but for one buffer the CPU writes and nothing
/// else binds; marks says which branch each case ran. Metal has no predication on render or blit encoders (no such
/// selector on 21H461 or 24B119) and either one ends the unit, so the draw, clear and blit are the work after a block.
struct PredicationTest: HWTest {
    let id = "gpu.raw.predication"
    let name = "Predicated dispatches on render enable: every compare, nesting, spanning encoders, work after, loops"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias StartIf = @convention(c) (AnyObject, Selector, MTLBuffer, Int, Int, UInt32) -> Void
    typealias Plain = @convention(c) (AnyObject, Selector) -> Void
    typealias End = @convention(c) (AnyObject, Selector) -> Bool
    static let startIf = NSSelectorFromString("encodeStartIf:offset:comparison:referenceValue:")
    static let startElse = NSSelectorFromString("encodeStartElse")
    static let endIf = NSSelectorFromString("encodeEndIf")

    struct Case {
        let word: UInt32, compare: Int, reference: UInt32
        var holds: Bool {
            switch compare {
            case 1: return word != reference
            case 2: return word > reference
            case 3: return word >= reference
            case 4: return word < reference
            case 5: return word <= reference
            default: return word == reference
            }
        }
    }

    static let pairs: [(UInt32, UInt32)] = [
        (5, 5), (4, 5), (6, 5), (0, 0), (0, 1), (1, 0), (0xffff_ffff, 0xffff_ffff), (0xffff_ffff, 0),
        (0x8000_0000, 0x7fff_ffff),
    ]
    static let cases: [Case] = (0..<7).flatMap { c in pairs.map { Case(word: $0.0, compare: c, reference: $0.1) } }
    static let nested: [(Bool, Bool)] = [(true, true), (true, false), (false, true), (false, false)]
    static let side = 16

    struct Encoding {
        let device: GPU
        let store: MTLComputePipelineState, mark: MTLComputePipelineState
        let words: MTLBuffer, marks: MTLBuffer, cpuWords: MTLBuffer
        var ends: [Bool] = []

        func dispatch(
            _ e: MTLComputeCommandEncoder, _ state: MTLComputePipelineState, _ buffer: MTLBuffer,
            _ at: Int, _ value: UInt32
        ) {
            var pair = SIMD2<UInt32>(UInt32(at), value)
            e.setComputePipelineState(state)
            e.setBuffer(buffer, offset: 0, index: 0)
            e.setBytes(&pair, length: 8, index: 1)
            e.dispatchThreadgroups(
                MTLSize(width: 1, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 1, height: 1, depth: 1))
        }

        func startIf(
            _ e: MTLComputeCommandEncoder, word: Int, offsetBias: Int = 0, _ compare: Int, _ ref: UInt32,
            in buffer: MTLBuffer? = nil
        ) {
            let o = e as AnyObject
            unsafeBitCast(o.method(for: PredicationTest.startIf), to: StartIf.self)(
                o, PredicationTest.startIf, buffer ?? words, word * 4 + offsetBias, compare, ref)
        }

        func startElse(_ e: MTLComputeCommandEncoder) {
            let o = e as AnyObject
            unsafeBitCast(o.method(for: PredicationTest.startElse), to: Plain.self)(o, PredicationTest.startElse)
        }

        mutating func endIf(_ e: MTLComputeCommandEncoder) {
            let o = e as AnyObject
            ends.append(unsafeBitCast(o.method(for: PredicationTest.endIf), to: End.self)(o, PredicationTest.endIf))
        }

        /// A GPU-written word, ordered before the record that reads it
        func word(_ e: MTLComputeCommandEncoder, _ at: Int, _ value: UInt32) {
            dispatch(e, store, words, at, value)
            e.memoryBarrier(scope: .buffers)
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard let probe = gpu.queue.makeCommandBuffer(), let compute = probe.makeComputeCommandEncoder() else {
            throw TestFailure("no command buffer or compute encoder")
        }
        let responds = (compute as AnyObject).responds(to: Self.startIf)
        compute.endEncoding()
        guard responds else { return .fail("the compute encoder answers no encodeStartIf:", Metrics()) }

        let caseCount = Self.cases.count, nestedMarks = Self.nested.count * 3
        var enc = Encoding(
            device: gpu, store: try gpu.compute("predication_store"), mark: try gpu.compute("predication_mark"),
            words: try gpu.buffer(4 * 256), marks: try gpu.buffer(4 * 256), cpuWords: try gpu.buffer(16))
        let cpu = enc.cpuWords.contents().bindMemory(to: UInt32.self, capacity: 4)
        cpu[0] = 42
        cpu[1] = 41
        let target = try gpu.texture(.bgra8Unorm, Self.side, Self.side, usage: [.renderTarget, .shaderRead])
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let cleared = try gpu.buffer(64), copied = try gpu.buffer(64)
        memset(enc.marks.contents(), 0, 4 * 256)
        memset(cleared.contents(), 0xab, 64)
        memset(copied.contents(), 0, 64)
        for _ in 0..<2 {
            _ = try await gpu.run { commands in self.encode(commands, &enc, target, solid, cleared, copied) }
        }
        memset(enc.marks.contents(), 0, 4 * 256)
        memset(cleared.contents(), 0xab, 64)
        memset(copied.contents(), 0, 64)
        enc.ends = []
        let ran = try await gpu.run { commands in self.encode(commands, &enc, target, solid, cleared, copied) }

        var metrics = Metrics()
        var wrong: [String] = []
        let marks = UnsafeBufferPointer(
            start: enc.marks.contents().bindMemory(to: UInt32.self, capacity: 256),
            count: 256)
        var compareWrong: [String] = []
        for (i, c) in Self.cases.enumerated() where marks[i] != (c.holds ? 1 : 2) {
            compareWrong.append("\(c.compare):\(c.word):\(c.reference)=\(marks[i])")
        }
        var nestedWrong = 0
        for (n, (outer, inner)) in Self.nested.enumerated() {
            let base = caseCount + 3 * n
            let want: [UInt32] = [outer && inner ? 1 : 0, outer && !inner ? 1 : 0, outer ? 0 : 1]
            for k in 0..<3 where marks[base + k] != want[k] { nestedWrong += 1 }
        }
        let span = marks[caseCount + nestedMarks]
        let spanFailed = marks[caseCount + nestedMarks + 1]
        let cpuHeld = marks[caseCount + nestedMarks + 2], cpuFailed = marks[caseCount + nestedMarks + 3]
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let drawn = stride(from: 0, to: pixels.count, by: 4).filter { pixels[$0 + 2] == 255 && pixels[$0 + 1] == 0 }
            .count
        let clearedOk = UnsafeRawBufferPointer(start: cleared.contents(), count: 64).allSatisfy { $0 == 0x5a }
        let copiedOk = UnsafeRawBufferPointer(start: copied.contents(), count: 64).allSatisfy { $0 == 0x5a }
        metrics.set("path", ran.path)
        metrics.set("compares_wrong", compareWrong.count)
        if !compareWrong.isEmpty { metrics.set("compares_first", compareWrong.prefix(4).joined(separator: ",")) }
        metrics.set("nested_wrong", nestedWrong)
        metrics.set("span_mark", Int(span))
        metrics.set("span_failed_mark", Int(spanFailed))
        metrics.set("cpu_word_marks", "\(cpuHeld)/\(cpuFailed)")
        metrics.set("draw_after_pixels", drawn)
        metrics.set("clear_after", clearedOk ? "ok" : "wrong")
        metrics.set("blit_after", copiedOk ? "ok" : "wrong")
        metrics.set("ends_yes", enc.ends.filter { $0 }.count)
        metrics.set("ends", enc.ends.count)
        metrics.set("gpu_ms", ran.gpuMilliseconds)
        if ran.path != "raw" { wrong.append("path") }
        if !compareWrong.isEmpty { wrong.append("compares") }
        if nestedWrong != 0 { wrong.append("nested") }
        if span != 1 || spanFailed != 0 { wrong.append("span") }
        if cpuHeld != 1 || cpuFailed != 0 { wrong.append("cpu_word") }
        if drawn != Self.side * Self.side { wrong.append("draw_after") }
        if !clearedOk { wrong.append("clear_after") }
        if !copiedOk { wrong.append("blit_after") }
        if enc.ends.contains(false) { wrong.append("ends") }
        var loops = try PredicationLoops(gpu)
        wrong += try await loops.run(&metrics)
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    func encode(
        _ commands: MTLCommandBuffer, _ enc: inout Encoding, _ target: MTLTexture,
        _ solid: MTLRenderPipelineState, _ cleared: MTLBuffer, _ copied: MTLBuffer
    ) {
        let e = commands.makeComputeCommandEncoder()!
        for (i, c) in Self.cases.enumerated() {
            enc.word(e, i, c.word)
            enc.startIf(e, word: i, offsetBias: i % 4 == 3 ? 2 : 0, c.compare, c.reference)
            enc.dispatch(e, enc.mark, enc.marks, i, 1)
            enc.startElse(e)
            enc.dispatch(e, enc.mark, enc.marks, i, 2)
            enc.endIf(e)
        }
        let base = Self.cases.count
        for (n, (outer, inner)) in Self.nested.enumerated() {
            let w = base + 2 * n, m = base + 3 * n
            enc.word(e, w, outer ? 7 : 8)
            enc.word(e, w + 1, inner ? 3 : 9)
            enc.startIf(e, word: w, 0, 7)
            enc.startIf(e, word: w + 1, 4, 5)
            enc.dispatch(e, enc.mark, enc.marks, m, 1)
            enc.startElse(e)
            enc.dispatch(e, enc.mark, enc.marks, m + 1, 1)
            enc.endIf(e)
            enc.startElse(e)
            enc.dispatch(e, enc.mark, enc.marks, m + 2, 1)
            enc.endIf(e)
        }
        let spanWord = base + 2 * Self.nested.count, spanMark = base + 3 * Self.nested.count
        enc.word(e, spanWord, 11)
        enc.startIf(e, word: spanWord, 2, 10)
        e.endEncoding()
        let second = commands.makeComputeCommandEncoder()!
        enc.dispatch(second, enc.mark, enc.marks, spanMark, 1)
        enc.endIf(second)
        enc.word(second, spanWord + 1, 0)
        enc.startIf(second, word: spanWord + 1, 0, 1)
        enc.dispatch(second, enc.mark, enc.marks, spanMark + 1, 1)
        enc.endIf(second)
        enc.startIf(second, word: 0, 0, 42, in: enc.cpuWords)
        enc.dispatch(second, enc.mark, enc.marks, spanMark + 2, 1)
        enc.endIf(second)
        enc.startIf(second, word: 1, 0, 42, in: enc.cpuWords)
        enc.dispatch(second, enc.mark, enc.marks, spanMark + 3, 1)
        enc.endIf(second)
        second.endEncoding()

        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        let r = commands.makeRenderCommandEncoder(descriptor: pass)!
        r.setRenderPipelineState(solid)
        GPU.draw(r, corners: GPU.fullScreen, color: SIMD4<Float>(1, 0, 0, 1))
        r.endEncoding()
        let blit = commands.makeBlitCommandEncoder()!
        blit.fill(buffer: cleared, range: 0..<64, value: 0x5a)
        blit.copy(from: cleared, sourceOffset: 0, to: copied, destinationOffset: 0, size: 64)
        blit.endEncoding()
    }
}
