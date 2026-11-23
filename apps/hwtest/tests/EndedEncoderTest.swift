import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Commands recorded on an encoder after its endEncoding, each case its own command buffer (gpu/EndedEncoderCases.swift),
/// against an M4 Max (validation off, as an app runs; every value below measured there): where nothing began after the
/// end Apple runs the command inside that encoder (late_draw, two_late, late_pipeline, late_buffer, late_bytes,
/// bind_only, then_load, parallel_sub, parallel_next, blit_late_copy, blit_earlier, late_dispatch, late_kernel,
/// late_kernel_buf: completed, T's corners and the buffers' bytes and words as the table says); where Apple's driver
/// crashes the process (draw_in_blit, copy_after_pass, copy_after_event, end_twice_render, end_twice_blit,
/// end_twice_compute, begin_while_open) the driver fails the command buffer InvalidResource (code 9, error:none), seven
/// failed.none; parallel_after (a sub-encoder's draw after its parent ended), which Apple crashes on and the driver
/// cannot fail, completes having drawn nothing and counts one skipped.enc.pass. <case>_status, <case>_off (values not
/// the table's).
struct EndedEncoderTest: HWTest {
    let id = "gpu.gm20b.ended_encoder"
    let name = "A command on an ended encoder runs inside it as Apple's does, or fails where Apple's crashes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 60 }

    typealias Reading = EndedEncoderBench.Reading
    static let red: [UInt8] = [255, 0, 0, 255], blue: [UInt8] = [0, 0, 255, 255], green: [UInt8] = [0, 255, 0, 255]
    static let white: [UInt8] = [255, 255, 255, 255], darkRed: [UInt8] = [128, 0, 0, 255]
    static let grey: [UInt8] = [128, 128, 128, 255]
    static let crashes = [
        "draw_in_blit", "copy_after_pass", "copy_after_event", "end_twice_render", "end_twice_blit",
        "end_twice_compute",
        "begin_while_open",
    ]

    /// T's corners tl, tr, bl, br
    static func corners(_ tl: [UInt8], _ tr: [UInt8], _ bl: [UInt8], _ br: [UInt8]) -> [String: [UInt8]] {
        ["tl": tl, "tr": tr, "bl": bl, "br": br]
    }

    static func word(_ bytes: [UInt8], _ i: Int) -> Int {
        (0 ..< 4).reduce(0) { $0 | Int(bytes[4 * i + $1]) << (8 * $1) }
    }

    /// Each expectation a case's reading misses
    static func misses(_ name: String, _ r: Reading) -> [String] {
        var misses: [String] = []
        let near = { (a: [UInt8], b: [UInt8]) in
            a.count == b.count && zip(a, b).allSatisfy { abs(Int($0) - Int($1)) <= 1 } }
        func pixels(_ want: [String: [UInt8]], snap: Bool = false) {
            for (key, value) in want
                where !near(r.pixels[key] ?? [], value) { misses.append("\(key) \(r.pixels[key] ?? [])") }
            if snap { for (key, value) in want where !near(r.snap[key] ?? [], value) { misses.append("snap_\(key)") } }
        }
        func bytes(_ buffer: [UInt8], _ want: [Int: UInt8], _ label: String) {
            for (i, v) in want where buffer[i] != v { misses.append("\(label)[\(i)] \(buffer[i])") }
        }
        if crashes.contains(name) {
            if r.status != "error" || r.code != 9 || r
                .path != "error:none" { misses.append("\(r.status) \(r.code) \(r.path)") }
            return misses
        }
        if r.status != "completed" { misses.append("\(r.status) \(r.code) \(r.path)") }
        switch name {
        case "late_draw": pixels(corners(red, blue, red, blue), snap: true)
        case "two_late": pixels(corners(white, white, red, blue), snap: true)
        case "late_pipeline": pixels(corners(darkRed, blue, darkRed, blue), snap: true)
        case "late_buffer", "late_bytes": pixels(corners(green, red, green, red))
        case "bind_only": pixels(corners(blue, blue, blue, blue), snap: true)
        case "then_load", "parallel_next": pixels(corners(grey, grey, red, blue))
        case "parallel_sub": pixels(corners(red, blue, red, blue))
        case "parallel_after": pixels(["tl": blue, "bl": blue])
        case "blit_late_copy":
            let want: [Int: UInt8] = [
                0: 0xaa,
                31: 0xaa,
                32: 0x20,
                40: 0x28,
                63: 0x3f,
                64: 0,
                128: 0x55,
                159: 0x55,
                160: 0,
            ]
            bytes(r.out, want, "out")
            bytes(r.snapOut, want, "snap_out")
        case "blit_earlier": bytes(r.out, [0: 0xaa, 31: 0xaa, 32: 0x11, 63: 0x11], "out")
        case "late_dispatch":
            for (i, v) in [0: 12, 63: 12, 64: 0]
                where word(r.out, i) != v { misses.append("out[\(i)] \(word(r.out, i))") }
            if word(r.snapOut, 0) != 12 { misses.append("snap_out[0] \(word(r.snapOut, 0))") }
        case "late_kernel":
            for i in [0, 63] where word(r.out, i) != 4 { misses.append("out[\(i)] \(word(r.out, i))") }
        case "late_kernel_buf":
            if word(r.out, 0) != 3 || word(r.out2, 0) !=
                3 { misses.append("out[0] \(word(r.out, 0)) out2[0] \(word(r.out2, 0))") }
        default: misses.append("no expectations")
        }
        return misses
    }

    static let cases = [
        "late_draw", "two_late", "late_pipeline", "late_buffer", "late_bytes", "bind_only", "then_load", "parallel_sub",
        "parallel_next", "blit_late_copy", "blit_earlier", "late_dispatch", "late_kernel", "late_kernel_buf",
        "parallel_after",
    ] + crashes

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        let before = GM20BFacade.counters(of: gpu.device)
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let bench = try EndedEncoderBench(gpu)
            for name in Self.cases {
                let r = try await bench.run(name), misses = Self.misses(name, r)
                metrics.set(name + "_status", r.status)
                metrics.set(name + "_off", misses.count)
                checks.append(("\(name): \(misses.prefix(3).joined(separator: ", "))", misses.isEmpty))
            }
        } catch {
            checks.append(("\(GPU.describe(error))", false))
        }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        let counted = GM20BFacade.counters(of: gpu.device)
        let counts = FailedCounts(before: before, after: counted)
        checks.append(
            FailedCounts.judged(
                before: before,
                after: counted,
                expecting: ["none": Self.crashes.count],
                own: gpu.paths,
                &metrics
            )
        )
        checks.append(counts.skipCheck(expecting: ["enc.pass": 1]))
        return .judged(checks, metrics, proof: gpu.proof)
    }
}
