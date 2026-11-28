import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// The CPU a frame shaped as the home screen's costs the driver, on the thread that records it and on the submit
/// worker: `passes` passes of Core Animation's untextured fill over four targets that repeat each frame (kept pass
/// shapes), `draws` draws spread over them, each bound as Core Animation binds it (seven small setBytes) under a
/// scissor of its own, committed and waited for as the render server does (waitUntilScheduled). The recording
/// thread's time is its own CPU time around the recording; the worker's is the submit thread's CPU time over the
/// frames, encodes ahead of the commit included. Per pass, per draw and per bind come from frames with twice the
/// passes, twice the draws and twice the binds. Allocations: the worker's per encode (encode profile; encodes ahead
/// have none), and the blocks the process holds more after the frames. The ceilings are the frame CPU plan's: red
/// until the driver meets them.
struct FrameRecordCostTest: HWTest {
    let id = "gpu.frame.record_cost"
    let name = "A home-screen shaped frame's driver CPU per pass, draw and bind on the recording thread and the worker"
    var subsystem: Subsystem { .metal }
    static let passes = 20, draws = 125, frames = 48, warmups = 16, targets = 4, side = 256
    static let worker = "com.brunostjohn.switch.gm20b.submit"
    /// The frame CPU plan's targets for a home-screen frame (126 draws, 21 passes): a kept pass 5 us recording and
    /// 5 us encoding, a draw 1 us recording and 1.5 us encoding, a bind 0.3 us, about 1.2 ms a frame in all
    static let ceilings: [(metric: String, most: Double)] = [
        ("record_us_per_pass", 5), ("record_us_per_draw", 1), ("record_us_per_bind", 0.3),
        ("worker_us_per_pass", 5), ("worker_us_per_draw", 1.5), ("frame_us", 1200), ("worker_mallocs_per_encode", 0),
    ]

    struct Shape {
        let passes: Int, draws: Int, binds: Int
    }

    struct Cost {
        var record = 0.0, worker = 0.0, mallocs = 0.0
    }

    func run(_: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade (\(GM20BFacade.kind(of: gpu.device)))")
        }
        let library: MTLLibrary
        do {
            library = try gpu.device.makeLibrary(URL: CAState.library)
        } catch {
            return .skip("no QuartzCore library here (\(GPU.describe(error)))")
        }
        guard let line = CACorpus.state("PBGRAXm_A2Xhf") else { return .fail("CACorpus has no PBGRAXm_A2Xhf") }
        let pipeline = try CAState.pipeline(line, library: library, device: gpu.device)
        let targets = try (0 ..< Self.targets).map { _ in try gpu.texture(.bgra8Unorm, Self.side, Self.side) }
        let scratch = try gpu.texture(
            CACorpus.colorFormat(line, 1), Self.side, Self.side, usage: [.renderTarget], storage: .memoryless
        )
        guard Self.threadMicroseconds(named: Self.worker) != nil else {
            return .fail("no thread named \(Self.worker) in this process")
        }
        let encode = { (commands: MTLCommandBuffer, shape: Shape) in
            for pass in 0 ..< shape.passes {
                let descriptor = MTLRenderPassDescriptor()
                descriptor.colorAttachments[0].texture = targets[pass % Self.targets]
                descriptor.colorAttachments[0].loadAction = pass < Self.targets ? .clear : .load
                descriptor.colorAttachments[0].storeAction = .store
                descriptor.colorAttachments[1].texture = scratch
                descriptor.colorAttachments[1].loadAction = .clear
                descriptor.colorAttachments[1].storeAction = .dontCare
                let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor)!
                encoder.setRenderPipelineState(pipeline)
                let mine = shape.draws / shape.passes + (pass < shape.draws % shape.passes ? 1 : 0)
                for draw in 0 ..< mine {
                    let x = Float(draw % 8) / 4 - 1, y = Float(pass % 6) / 3 - 1
                    let colour = SIMD4<Float16>(Float16(draw % 4) / 4, Float16(pass % 8) / 8, 0.5, 1)
                    let vertices = CAState.rectangle(x0: x, y0: y, x1: x + 0.25, y1: y + 0.3, color: colour)
                    for _ in 0 ..< shape.binds { CAState.bind(encoder, vertices: vertices, source: nil) }
                    let scissor = MTLScissorRect(x: (draw % 8) * 32, y: (pass % 8) * 32, width: 32, height: 32)
                    encoder.setScissorRect(scissor)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                }
                encoder.endEncoding()
            }
        }
        let measure = { (shape: Shape) throws -> Cost in
            for _ in 0 ..< Self.warmups {
                try gpu.finish([try gpu.commit { encode($0, shape) }])
            }
            let profiled = GM20BFacade.encodeProfile(of: gpu.device)
            let workerBefore = Self.threadMicroseconds(named: Self.worker) ?? 0
            var recorded = 0.0
            var held: [MTLCommandBuffer] = []
            for _ in 0 ..< Self.frames {
                try autoreleasepool {
                    let commands = gpu.queue.makeCommandBuffer()!
                    let before = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)
                    encode(commands, shape)
                    commands.commit()
                    commands.waitUntilScheduled()
                    recorded += Double(clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - before) / 1000
                    held.append(commands)
                    if held.count == 3 { try gpu.finish(held); held.removeAll() }
                }
            }
            try gpu.finish(held)
            let workerAfter = Self.threadMicroseconds(named: Self.worker) ?? 0
            let after = GM20BFacade.encodeProfile(of: gpu.device)
            let encodes = max((after["malloc_encodes"] ?? 0) - (profiled["malloc_encodes"] ?? 0), 1)
            return Cost(
                record: recorded / Double(Self.frames), worker: (workerAfter - workerBefore) / Double(Self.frames),
                mallocs: ((after["mallocs"] ?? 0) - (profiled["mallocs"] ?? 0)) / encodes
            )
        }
        let base = Shape(passes: Self.passes, draws: Self.draws, binds: 1)
        let blocks = EncodeCostPathTest.blocksInUse()
        let frame = try measure(base)
        let heldBlocks = (EncodeCostPathTest.blocksInUse() - blocks) / Double(Self.frames + Self.warmups)
        let morePasses = try measure(Shape(passes: 2 * Self.passes, draws: Self.draws, binds: 1))
        let moreDraws = try measure(Shape(passes: Self.passes, draws: 2 * Self.draws, binds: 1))
        let moreBinds = try measure(Shape(passes: Self.passes, draws: Self.draws, binds: 2))
        let bindsPerDraw = 7.0
        var metrics = Metrics()
        let per: [(String, Double)] = [
            ("record_us_per_pass", (morePasses.record - frame.record) / Double(Self.passes)),
            ("record_us_per_draw", (moreDraws.record - frame.record) / Double(Self.draws)),
            ("record_us_per_bind", (moreBinds.record - frame.record) / (Double(Self.draws) * bindsPerDraw)),
            ("worker_us_per_pass", (morePasses.worker - frame.worker) / Double(Self.passes)),
            ("worker_us_per_draw", (moreDraws.worker - frame.worker) / Double(Self.draws)),
            ("worker_us_per_bind", (moreBinds.worker - frame.worker) / (Double(Self.draws) * bindsPerDraw)),
            ("frame_us", frame.record + frame.worker), ("worker_mallocs_per_encode", frame.mallocs),
        ]
        metrics.set("record_us", frame.record, digits: 0)
        metrics.set("worker_us", frame.worker, digits: 0)
        metrics.set("held_blocks_per_frame", heldBlocks, digits: 1)
        for (key, value) in per { metrics.set(key, value, digits: 2) }
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        var failures: [String] = []
        if gpu.paths.paths.keys.contains(where: { $0 != "gm20b" }) {
            failures.append("command buffers went \(gpu.paths.text)")
        }
        let measured = Dictionary(uniqueKeysWithValues: per)
        for (metric, most) in Self.ceilings where (measured[metric] ?? 0) > most {
            failures.append("\(metric) \(String(format: "%.2f", measured[metric] ?? 0)) over \(most)")
        }
        return failures.isEmpty
            ? .pass(metrics, proof: gpu.proof) : .fail(failures.joined(separator: "; "), metrics, proof: gpu.proof)
    }

    /// A thread of this process's CPU time (user and system) in microseconds, found by its name
    static func threadMicroseconds(named name: String) -> Double? {
        var list: thread_act_array_t?
        var count: mach_msg_type_number_t = 0
        guard task_threads(mach_task_self_, &list, &count) == KERN_SUCCESS, let threads = list else { return nil }
        defer {
            for i in 0 ..< Int(count) { mach_port_deallocate(mach_task_self_, threads[i]) }
            vm_deallocate(
                mach_task_self_, vm_address_t(UInt(bitPattern: threads)),
                vm_size_t(Int(count) * MemoryLayout<thread_t>.stride)
            )
        }
        for i in 0 ..< Int(count) {
            var info = thread_extended_info()
            var size = mach_msg_type_number_t(MemoryLayout<thread_extended_info>.size / MemoryLayout<natural_t>.size)
            let kr = withUnsafeMutablePointer(to: &info) {
                $0.withMemoryRebound(to: integer_t.self, capacity: Int(size)) {
                    thread_info(threads[i], thread_flavor_t(THREAD_EXTENDED_INFO), $0, &size)
                }
            }
            guard kr == KERN_SUCCESS else { continue }
            let named = withUnsafeBytes(of: info.pth_name) {
                String(cString: $0.bindMemory(to: CChar.self).baseAddress!)
            }
            if named == name { return Double(info.pth_user_time + info.pth_system_time) / 1000 }
        }
        return nil
    }
}
