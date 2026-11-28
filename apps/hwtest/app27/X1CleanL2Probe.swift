import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// Host1x syncpoints a debug kernel hands HW Test probes (TegraGM20BPathsABI.h TegraGM20BPathsOpProbeSyncpts): taken at
/// the first ask, never waited on, so a probe's 3D-engine increments disturb no consumer.
enum GM20BProbeSyncpoints {
    static let op: UInt32 = 7

    static let kIOReturnNotPrivileged = kern_return_t(bitPattern: 0xe000_02c1)

    static func read(_ connection: GPUConnection?) -> (ids: [UInt32], values: [UInt32])? {
        guard let connection else { return nil }
        let (kr, reply) = connection.call(GPUPaths.method, GPUConnection.words([op, 0]), outputSize: GPUPaths.replySize)
        guard kr == KERN_SUCCESS else { return nil }
        var ids: [UInt32] = [], values: [UInt32] = []
        for field in String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self).split(separator: " ") {
            let pair = field.split(separator: "=", maxSplits: 1)
            guard pair.count == 2 else { continue }
            let numbers = pair[1].split(separator: ",").compactMap { UInt32($0) }
            if pair[0] == "ids" { ids = numbers } else if pair[0] == "values" { values = numbers }
        }
        return ids.isEmpty || ids.contains(0) || ids.count != values.count ? nil : (ids, values)
    }

    /// Why `read` gave nothing: only a release kernel's answer is a reason to skip; a refusal of HW Test's own
    /// entitlement (SWITCH_GPU_PROBES_ENTITLEMENT) or any other answer is a failure.
    static func absence(_ connection: GPUConnection?) -> ProbeFinding {
        guard let connection else { return ProbeFinding(.fail, note: "no TegraGM20B connection") }
        let (kr, _) = connection.call(GPUPaths.method, GPUConnection.words([op, 0]), outputSize: GPUPaths.replySize)
        switch kr {
        case GPUConnection.kIOReturnUnsupported:
            return ProbeFinding(.inconclusive, note: "no probe syncpoints (a release kernel)")
        case kIOReturnNotPrivileged:
            return ProbeFinding(.fail, note: "the kernel refused HW Test's gpu-probes entitlement")
        default:
            return ProbeFinding(.fail, note: "no probe syncpoints (\(GPUConnection.hex(kr)))")
        }
    }
}

/// learn/deko3d.md X1, probe P9: does the 3D engine's INCREMENT_SYNC_POINT with CLEAN_L2 and ROP_WRITES_DONE put the
/// ROP's writes in DRAM, as NVN's flushing fence relies on? Each trial draws a value of its own into a pitch-linear
/// target over a private buffer (L2-cached and never compressed; its job is internal, so the kernel's fence flushes no
/// L2); a blit encoder then pushes FLUSH_PENDING_WRITES and the increment twice on a probe syncpoint (the control arm
/// pushes neither); once it completed, the CPU reads the buffer through the facade's raw read, past the driver's
/// visibility work. clean_wrong: clean trials with a wrong byte (0); clean_steps: the syncpoint's moves (2 a clean
/// trial); control_stale: control trials that read stale, showing the L2 still held the writes the clean wrote back.
/// Other processes' fences flush the L2 too, so a control that reads exact fails nothing.
enum X1CleanL2Probe {
    static let probe = X1Probe(
        id: "gpu.x1.clean_l2_syncpoint",
        name: "A 3D-engine syncpoint increment with CLEAN_L2 puts the ROP's writes in DRAM for the CPU",
        probe: "p9.clean_l2", risky: true, timeout: 120, body: run
    )

    static let trials = 16
    static let (width, height) = (256, 64)
    static let flushPendingWrites = 0x1144, incrementSyncPoint = 0x02c8
    static let cleanL2: UInt32 = 1 << 16, ropWritesDone: UInt32 = 1 << 20
    static let rawReadSelector = NSSelectorFromString("switchMetalProbeRawRead:")

    static func run(_ gpu: GPU) async throws -> ProbeFinding {
        var metrics = Metrics()
        let connection = GPUConnection()
        guard let before = GM20BProbeSyncpoints.read(connection)
        else { return GM20BProbeSyncpoints.absence(connection) }
        let syncpoint = before.ids[0]
        let bytesPerRow = width * 4
        guard let buffer = gpu.device.makeBuffer(length: bytesPerRow * height, options: .storageModePrivate) else {
            throw TestFailure("no private buffer")
        }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: width, height: height, mipmapped: false
        )
        descriptor.storageMode = .private
        descriptor.usage = [.renderTarget]
        guard let target = buffer.makeTexture(descriptor: descriptor, offset: 0, bytesPerRow: bytesPerRow) else {
            throw TestFailure("no pitch-linear target over the private buffer")
        }
        let state = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        var cleanWrong = 0, controlStale = 0, steps = 0, cpuView = "-"
        var firsts: [String] = []
        for trial in 0 ..< trials {
            for clean in [true, false] {
                let value = UInt8(truncatingIfNeeded: 1 + trial * 2 + (clean ? 0 : 1))
                let at = GM20BProbeSyncpoints.read(connection)?.values[0] ?? 0
                try await gpu.run { commands in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = target
                    pass.colorAttachments[0].loadAction = .dontCare
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(state)
                    var corners = GPU.fullScreen
                    encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                    var colour = SIMD4<Float>(repeating: Float(value) / 255)
                    encoder.setFragmentBytes(&colour, length: 16, index: 0)
                    encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                    encoder.endEncoding()
                    let blit = commands.makeBlitCommandEncoder()!
                    if clean {
                        try GM20BProbe.engine(
                            commands, ["kind": "method", "method": flushPendingWrites, "value": 0, "written": target]
                        )
                        for _ in 0 ..< 2 {
                            try GM20BProbe.engine(
                                commands,
                                [
                                    "kind": "method", "method": incrementSyncPoint,
                                    "value": syncpoint | cleanL2 | ropWritesDone, "written": target,
                                ]
                            )
                        }
                    }
                    blit.endEncoding()
                }
                let (bytes, view) = try rawRead(gpu.device, buffer)
                cpuView = view
                let wrong = bytes.filter { $0 != value }.count
                if clean {
                    steps += Int((GM20BProbeSyncpoints.read(connection)?.values[0] ?? at) &- at)
                    cleanWrong += wrong == 0 ? 0 : 1
                    if wrong != 0, firsts.count < 3 { firsts.append("t\(trial):\(wrong)") }
                } else {
                    controlStale += wrong == 0 ? 0 : 1
                }
            }
        }
        metrics.set("syncpoint", Int(syncpoint))
        metrics.set("cpu_view", cpuView)
        metrics.set("trials", trials)
        metrics.set("clean_steps", steps)
        metrics.set("control_stale", controlStale)
        metrics.set("first", firsts.isEmpty ? "-" : firsts.joined(separator: ";"))
        var wrong = ["clean_wrong": cleanWrong]
        wrong["clean_steps_off"] = steps == 2 * trials ? 0 : 1
        return ProbeFinding.counted(wrong, paths: gpu.paths.paths.keys.map { $0 }, metrics: metrics)
    }

    /// The buffer's bytes as the CPU sees them in its own mapping, with none of the driver's visibility work, and that
    /// mapping's kind ("wc": uncached; "cached": cleaned and invalidated first)
    static func rawRead(_ device: MTLDevice, _ buffer: MTLBuffer) throws -> ([UInt8], String) {
        guard let object = device as? NSObject, object.responds(to: rawReadSelector),
              let answer = object.perform(rawReadSelector, with: buffer)?.takeUnretainedValue() as? NSDictionary,
              let data = answer["bytes"] as? Data
        else { throw TestFailure("the facade has no switchMetalProbeRawRead: (a release plug-in or another device)") }
        return ([UInt8](data), (answer["view"] as? String) ?? "-")
    }
}
