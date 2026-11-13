import Foundation
import HWTestCore
import IOSurface
import Metal
import SwitchBench

/// The bench's raw device (hw_test_app/bench/switch_bench.h): one experiment at a time, on its caller's thread.
final class BenchRunner {
    private let bench: OpaquePointer

    private init(_ bench: OpaquePointer) { self.bench = bench }

    deinit { switch_bench_close(bench) }

    /// The Metal device is made first: it loads the GPU plugin, beside which the bench finds the raw pack.
    static func open() throws -> BenchRunner {
        _ = MTLCreateSystemDefaultDevice()
        var why = [CChar](repeating: 0, count: 256)
        guard let bench = switch_bench_open(&why, why.count) else { throw BenchInvalid(String(cString: why)) }
        return BenchRunner(bench)
    }

    var summary: String {
        var line = [CChar](repeating: 0, count: 512)
        switch_bench_describe(bench, &line, line.count)
        return String(cString: line)
    }

    static var knownData: [UInt32] {
        var words = [UInt32](repeating: 0, count: 4)
        switch_bench_known_data(&words)
        return words
    }

    static var knownKernel: Data {
        var bytes = [UInt8](repeating: 0, count: 256)
        return Data(bytes.prefix(switch_bench_known_kernel(&bytes, bytes.count)))
    }

    static let kinds: [BenchKind: switch_bench_kind] = [
        .noop: SWITCH_BENCH_NOOP, .known: SWITCH_BENCH_KNOWN, .fragment: SWITCH_BENCH_FRAGMENT,
        .compute: SWITCH_BENCH_COMPUTE, .readback: SWITCH_BENCH_READBACK, .twod: SWITCH_BENCH_TWOD,
        .pack: SWITCH_BENCH_PACK,
    ]
    static let packForms: [String: switch_bench_pack_form] = [
        "whole": SWITCH_BENCH_PACK_WHOLE, "whole_generic": SWITCH_BENCH_PACK_WHOLE_GENERIC,
        "split": SWITCH_BENCH_PACK_SPLIT, "split_whole": SWITCH_BENCH_PACK_SPLIT_WHOLE,
    ]
    /// The most a split's frontier holds (raw_split_metadata.h): the room a pack experiment's quads need for theirs
    static let frontierBytes = 4096
    static let statuses = ["ok", "invalid", "refused", "killed", "error"]
    static let twodOps: [BenchTwodOp: switch_bench_twod_op] = [
        .fill: SWITCH_BENCH_TWOD_FILL, .scale: SWITCH_BENCH_TWOD_SCALE, .resolve: SWITCH_BENCH_TWOD_RESOLVE,
    ]
    static let compares: [String: switch_bench_compare] = [
        "program": SWITCH_BENCH_COMPARE_PROGRAM, "copy": SWITCH_BENCH_COMPARE_COPY,
        "resolve3d": SWITCH_BENCH_COMPARE_RESOLVE3D, "plain": SWITCH_BENCH_COMPARE_PLAIN,
        "sampled3d": SWITCH_BENCH_COMPARE_SAMPLED3D,
    ]

    /// All of `surface` (one plane, of a format the raw path renders) filled with `color` (an RGBA8 word) by the 2D engine
    /// on the bench's own connection and channel, waited for: "ok", else the status and why.
    func fill(_ surface: IOSurfaceRef, format: MTLPixelFormat, color: UInt32) -> String {
        var r = switch_bench_result()
        switch_bench_fill_surface(
            bench, Unmanaged.passUnretained(surface).toOpaque(), UInt32(format.rawValue), color, &r)
        let status = Self.statuses[Int(r.status)]
        return status == "ok" ? status : "\(status):\(Self.text(&r.why))"
    }

    func run(_ e: BenchExperiment) -> BenchOutcome {
        let started = DispatchTime.now().uptimeNanoseconds
        let pinned = Pinned()
        let program = e.program ?? BenchProgram(sph: Data(), code: Data(), data: Data(), gprs: 0)
        var x = switch_bench_experiment()
        x.kind = Self.kinds[e.kind]!.rawValue
        (x.format, x.width, x.height) = (e.formatRaw, e.width, e.height)
        x.rect = (e.rect[0], e.rect[1], e.rect[2], e.rect[3])
        x.clear = (e.clear[0], e.clear[1], e.clear[2], e.clear[3])
        x.fill = e.fill.flatMap(pinned.bytes)
        x.program = switch_bench_program(
            sph: pinned.bytes(program.sph), code: pinned.bytes(program.code), code_bytes: program.code.count,
            data: pinned.bytes(program.data), data_bytes: program.data.count, gprs: program.gprs,
            data_bound: program.dataBound)
        x.threads = (e.threads[0], e.threads[1], e.threads[2])
        x.groups = (e.groups[0], e.groups[1], e.groups[2])
        (x.draws, x.draw_counts) = (pinned.words(e.draws), UInt32(e.draws.count))
        (x.repeats, x.warmup) = (e.repeats, e.warmup)
        (x.methods, x.method_pairs) = (pinned.words(e.methods), UInt32(e.methods.count / 2))
        (x.restore, x.restore_pairs) = (pinned.words(e.restore), UInt32(e.restore.count / 2))
        x.single_target = e.singleTarget ? 1 : 0
        x.template_key = e.templateKey
        if let t = e.twod {
            x.twod.op = Self.twodOps[t.op]!.rawValue
            (x.twod.fmt2d, x.twod.color_format) = (t.fmt2d, t.colorFormat)
            x.twod.color = (t.color[0], t.color[1], t.color[2], t.color[3])
            (x.twod.du, x.twod.dv, x.twod.sx, x.twod.sy) = (t.du, t.dv, t.sx, t.sy)
            (x.twod.bilinear, x.twod.corner) = (t.bilinear ? 1 : 0, t.corner ? 1 : 0)
            (x.twod.reference, x.twod.bracketed, x.twod.samples) = (t.reference ? 1 : 0, t.bracketed ? 1 : 0, t.samples)
            if let c = t.clip { x.twod.clip = (c[0], c[1], c[2], c[3]) }
            x.twod.clear_first = t.clearFirst ? 1 : 0
            x.twod.tiled = t.tiled ? 1 : 0
            x.twod.resolve_by_scale = t.byScale ? 1 : 0
        }
        if let s = e.source {
            x.source = switch_bench_source(
                format: s.formatRaw, width: s.width, height: s.height, data: pinned.bytes(s.data))
        }
        x.compare = (e.compare.flatMap { Self.compares[$0] } ?? SWITCH_BENCH_COMPARE_NONE).rawValue
        x.compressed = e.compressed ? 1 : 0
        x.output_bytes = UInt32(e.outputBytes)
        x.chain = pinned.programs(
            e.chain.map { link in
                switch_bench_program(
                    sph: nil, code: pinned.bytes(link.code), code_bytes: link.code.count, data: pinned.bytes(link.data),
                    data_bytes: link.data.count, gprs: link.gprs, data_bound: 0)
            })
        (x.chain_links, x.chain_schedule) = (UInt32(e.chain.count), e.chainSchedule)
        if let p = e.pack {
            let quads = p.quads.map { quad in
                let blocks = quad.blocks.map {
                    switch_bench_block(
                        stage: $0.stage, slot: $0.slot, offset: $0.offset, bytes: UInt32($0.data.count),
                        data: pinned.bytes($0.data))
                }
                return switch_bench_quad(
                    rect: (quad.rect[0], quad.rect[1], quad.rect[2], quad.rect[3]), blocks: pinned.values(blocks),
                    block_count: UInt32(blocks.count), vertices: quad.vertices.flatMap(pinned.bytes),
                    vertex_bytes: UInt32(quad.vertices?.count ?? 0), primitive: quad.primitive, first: quad.first,
                    count: quad.count)
            }
            x.pack = switch_bench_pack(
                form: Self.packForms[p.form]!.rawValue, ca_transform: p.ca ? 1 : 0, quads: pinned.values(quads),
                quad_count: UInt32(quads.count))
        }
        (x.storage_bytes, x.storage_va_at) = (e.storage?.bytes ?? 0, e.storage?.vaAt ?? 0)
        let timed = e.draws.count * Int(e.repeats)
        let texelBytes = e.readback == .none ? 0 : Int(e.width * e.height) * e.texelBytes
        var r = switch_bench_result()
        r.gpu_ns = pinned.zeroed(UInt64.self, timed)
        r.wall_ns = pinned.zeroed(UInt64.self, timed)
        r.texels = texelBytes > 0 ? pinned.zeroed(UInt8.self, texelBytes) : nil
        r.texel_bytes = texelBytes
        r.compare_texels = texelBytes > 0 && e.compare != nil ? pinned.filled(UInt8.self, texelBytes, Self.unread) : nil
        r.output = e.outputBytes > 0 ? pinned.filled(UInt8.self, e.outputBytes, Self.unread) : nil
        r.output_bytes = e.outputBytes
        let frontierRoom = (e.pack?.quads.count ?? 0) * Self.frontierBytes
        r.frontier = frontierRoom > 0 ? pinned.zeroed(UInt8.self, frontierRoom) : nil
        r.frontier_bytes = frontierRoom
        let storageBytes = Int(e.storage?.bytes ?? 0)
        r.storage = storageBytes > 0 ? pinned.zeroed(UInt8.self, storageBytes) : nil
        r.storage_bytes = storageBytes
        switch_bench_run(bench, &x, &r)
        var outcome = BenchOutcome(status: Self.statuses[Int(r.status)], why: Self.text(&r.why))
        let gpu = Array(UnsafeBufferPointer(start: r.gpu_ns, count: timed))
        let wall = Array(UnsafeBufferPointer(start: r.wall_ns, count: timed))
        let repeats = Int(e.repeats)
        if outcome.status == "ok" {
            outcome.runs = e.draws.enumerated().map { index, draws in
                let slice = (index * repeats)..<((index + 1) * repeats)
                return (draws, Array(gpu[slice]), Array(wall[slice]))
            }
            if let texels = r.texels { outcome.texels = Data(bytes: texels, count: texelBytes) }
            if let other = r.compare_texels, r.compare_status == SWITCH_BENCH_OK.rawValue {
                outcome.compareTexels = Data(bytes: other, count: texelBytes)
            }
            if let output = r.output { outcome.output = Data(bytes: output, count: e.outputBytes) }
            if let frontier = r.frontier, e.pack != nil {
                outcome.split = (Int(r.launches), Data(bytes: frontier, count: r.frontier_bytes))
            }
            if let storage = r.storage { outcome.storage = Data(bytes: storage, count: storageBytes) }
        }
        if r.form.set != 0 {
            outcome.form = [
                "comptags": Int(r.form.comptags), "block_linear": Int(r.form.block_linear),
                "bracketed": Int(r.form.bracketed), "reference": Int(r.form.reference), "samples": Int(r.form.samples),
                "clipped": Int(r.form.clipped), "clear_first": Int(r.form.clear_first),
            ]
        }
        (outcome.lostError, outcome.faultVA, outcome.templateKey) = (r.lost_error, r.fault_va, r.template_key)
        if e.compare != nil, r.compare_status != SWITCH_BENCH_OK.rawValue {
            outcome.compareFailure = (Self.statuses[Int(r.compare_status)], Self.text(&r.compare_why))
        }
        outcome.milliseconds = Double(DispatchTime.now().uptimeNanoseconds - started) / 1e6
        return outcome
    }

    /// What the app's copies of a compare target and an output buffer hold until the GPU's bytes are copied in
    static let unread: UInt8 = 0xEE

    private static func text<T>(_ tuple: inout T) -> String {
        withUnsafeBytes(of: &tuple) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
    }
}

/// Copies of the inputs at addresses that stay put for the C call, freed with it.
private final class Pinned {
    private var buffers: [UnsafeMutableRawBufferPointer] = []

    deinit { buffers.forEach { $0.deallocate() } }

    func bytes(_ data: Data) -> UnsafePointer<UInt8>? {
        guard !data.isEmpty else { return nil }
        let buffer = allocate(data.count)
        buffer.copyBytes(from: data)
        return UnsafePointer(buffer.baseAddress!.assumingMemoryBound(to: UInt8.self))
    }

    func words(_ values: [UInt32]) -> UnsafePointer<UInt32>? {
        guard !values.isEmpty else { return nil }
        let buffer = allocate(values.count * 4)
        values.withUnsafeBytes { buffer.copyMemory(from: $0) }
        return UnsafePointer(buffer.baseAddress!.assumingMemoryBound(to: UInt32.self))
    }

    func programs(_ values: [switch_bench_program]) -> UnsafePointer<switch_bench_program>? {
        self.values(values)
    }

    func values<T>(_ values: [T]) -> UnsafePointer<T>? {
        guard !values.isEmpty else { return nil }
        let buffer = allocate(values.count * MemoryLayout<T>.stride)
        values.withUnsafeBytes { buffer.copyMemory(from: $0) }
        return UnsafePointer(buffer.baseAddress!.assumingMemoryBound(to: T.self))
    }

    func zeroed<T>(_: T.Type, _ count: Int) -> UnsafeMutablePointer<T> {
        filled(T.self, count, 0)
    }

    func filled<T>(_: T.Type, _ count: Int, _ byte: UInt8) -> UnsafeMutablePointer<T> {
        let buffer = allocate(max(1, count) * MemoryLayout<T>.stride)
        buffer.initializeMemory(as: UInt8.self, repeating: byte)
        return buffer.baseAddress!.assumingMemoryBound(to: T.self)
    }

    private func allocate(_ count: Int) -> UnsafeMutableRawBufferPointer {
        let buffer = UnsafeMutableRawBufferPointer.allocate(byteCount: count, alignment: 16)
        buffers.append(buffer)
        return buffer
    }
}
