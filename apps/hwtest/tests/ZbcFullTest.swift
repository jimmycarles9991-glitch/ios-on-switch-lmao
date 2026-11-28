import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import HWTestShared
import Metal

/// The GPU's ZBC colour table filling up (drivers/TegraGM20BMetal/zbc.h: 15 colour entries, every
/// process's, never emptied; comp.cpp zbcMake): 16 opaque BGRA8 colours of exact codes no other test clears to,
/// each cleared three times in full over a private 512x512 target on compressed memory (the third clear asks for the
/// entry). The compression thread answers the asks; the test waits for its settled event, which comes once every entry
/// asked is added or refused (CompressionSettled; `drained` 1, 0 where the facade has no event). Then each colour is
/// cleared once more and read back. Judged: every texel of each colour's third and fourth clear its codes
/// (`pixels_wrong` 0); where drained, the table answered every colour this process had not asked before (`zbc_added` +
/// `zbc_full` = `asked_want`: 16 on the process's first run, 0 after) and refused at least one of them (`zbc_full` >=
/// 1: the table holds 15, its boot entries among them, so 16 new colours overflow it however full it already was).
/// Reported: zbc_hits, full_lines (this process's "table is full" lines since the test began, as logd had them; the
/// pulled log lists them by process: raw.zbc-full-lines), and where drained zbc_dropped, the colours neither added nor
/// refused, with zbc_inexact and zbc_misses over the test: a colour is dropped before any ask where its clear is not
/// exact (zbc_inexact) or where this process's list of values (zbc.h ZbcTable, 64, evicting seen and refused entries
/// while keeping held, wanted, and asking) has no room left by the tests before it.
struct ZbcFullTest: HWTest {
    let id = "gpu.gm20b.zbc_full"
    let name = "The GPU's ZBC colour table fills: every clear exact, the refusals counted, one line a process"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 90 }

    static let side = 512, colours = 16
    static let fullLine = "the GPU's ZBC colour table is full"

    /// Colour k's BGRA8 bytes: red 3 + 16k, green 250 - 15k, blue 40 + 13k, opaque
    static func bytes(_ k: Int) -> [UInt8] { [UInt8(40 + 13 * k), UInt8(250 - 15 * k), UInt8(3 + 16 * k), 255] }

    static func clearColour(_ k: Int) -> MTLClearColor {
        let b = bytes(k)
        return MTLClearColor(red: Double(b[2]) / 255, green: Double(b[1]) / 255, blue: Double(b[0]) / 255, alpha: 1)
    }

    final class Runs: @unchecked Sendable {
        static let shared = Runs()
        private let lock = NSLock()
        private var count = 0

        func next() -> Int {
            lock.lock()
            defer { lock.unlock() }
            count += 1
            return count
        }
    }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard !GM20BFacade.compression(of: gpu.device).isEmpty,
              (gpu.device as? NSObject)?.responds(to: CompressionSettled.factsSelector) == true
        else { return .skip(GM20BFacade.missing("switchMetalCompression", on: gpu.device)) }
        let start = Date()
        let run = Runs.shared.next()
        var metrics = Metrics()
        let target = try await CompressionSettled.remade(gpu.device) {
            try gpu.texture(.bgra8Unorm, Self.side, Self.side)
        }
        let compressed = CompressionSettled.comptags(gpu.device, target) ?? -1
        metrics.set("target_comptags", compressed)
        guard compressed == 1 else { return .fail("no compressed 512x512 BGRA8 target", metrics) }
        let before = GM20BFacade.compression(of: gpu.device)
        let readback = try gpu.buffer(Self.side * Self.side * 4)
        var pixelsWrong = 0
        for k in 0 ..< Self.colours {
            pixelsWrong += try Self.clearAndCheck(gpu, target, readback: readback, colour: k, repeats: 3)
        }
        let drained = try await Self.drain(gpu)
        let answered = GM20BFacade.compression(of: gpu.device)
        for k in 0 ..< Self.colours {
            pixelsWrong += try Self.clearAndCheck(gpu, target, readback: readback, colour: k, repeats: 1)
        }
        let after = GM20BFacade.compression(of: gpu.device)
        let delta = { (a: [String: Int], b: [String: Int], key: String) in (b[key] ?? 0) - (a[key] ?? 0) }
        let added = delta(before, answered, "zbc_added"), full = delta(before, answered, "zbc_full")
        let want = run == 1 ? Self.colours : 0
        metrics.set("run", run)
        metrics.set("drained", drained)
        metrics.set("zbc_added", added)
        metrics.set("zbc_full", full)
        metrics.set("asked_want", want)
        metrics.set("zbc_hits", delta(before, after, "zbc_hits"))
        if drained { metrics.set("zbc_dropped", want - added - full) }
        metrics.set("zbc_inexact", delta(before, after, "zbc_inexact"))
        metrics.set("zbc_misses", delta(before, after, "zbc_misses"))
        metrics.set("pixels_wrong", pixelsWrong)
        let fullLines = ProcessLog.lines(ProcessLog.metal, "gm20b", containing: Self.fullLine, since: start)
        metrics.set("full_lines", fullLines?.count ?? -1)
        var checks = [("\(pixelsWrong) texels off their clear", pixelsWrong == 0)]
        if drained {
            checks.append(("the table answered \(added) + \(full) of \(want) colours", added + full == want))
            if want > 0 { checks.append(("the table refused at least one colour (\(full))", full >= 1)) }
        }
        return Outcome.judged(checks, metrics, proof: gpu.proof).onRawPath(gpu)
    }

    static func clearAndCheck(
        _ gpu: GPU, _ target: MTLTexture, readback: MTLBuffer, colour k: Int, repeats: Int
    ) throws -> Int {
        let color = clearColour(k)
        let rowBytes = Self.side * 4
        var buffers: [MTLCommandBuffer] = []
        buffers.reserveCapacity(repeats + 1)
        for _ in 0 ..< repeats {
            buffers.append(
                try gpu.commit { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .clear
                    pass.colorAttachments[0].clearColor = color
                    pass.colorAttachments[0].storeAction = .store
                    commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
                }
            )
        }
        buffers.append(
            try gpu.commit { commands in
                let blit = commands.makeBlitCommandEncoder()!
                blit.copy(
                    from: target, sourceSlice: 0, sourceLevel: 0, sourceOrigin: MTLOrigin(),
                    sourceSize: MTLSize(width: Self.side, height: Self.side, depth: 1), to: readback,
                    destinationOffset: 0, destinationBytesPerRow: rowBytes, destinationBytesPerImage: 0
                )
                blit.endEncoding()
            }
        )
        try gpu.finish(buffers)
        let b = bytes(k)
        let want = ByteCompare.word(b[0], b[1], b[2], b[3])
        return ByteCompare.wordWrongCount(
            UnsafeRawBufferPointer(start: readback.contents(), count: rowBytes * Self.side), want: want
        )
    }

    /// The compression thread settled: every ZBC entry asked so far added or refused; false where the facade has no
    /// event to wait for
    static func drain(_ gpu: GPU) async throws -> Bool {
        let settled = await CompressionSettled.wait(gpu.device)
        if let failure = settled.failure { throw failure }
        return settled != .absent
    }
}
