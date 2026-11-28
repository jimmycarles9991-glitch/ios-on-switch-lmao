import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// What Metal's validation layer would stop completes on the driver as Apple's driver completes it
/// (drivers/TegraGM20BMetal/client_errors.h). Every expected value is what an M-series Mac did with API
/// validation off (x1-m2v-errors' probes), each command buffer completed with no error; the driver logs one refusal
/// line a process and rule. Each case: status=completed, paths=raw, failed_cbs=0 and its contents.
enum ClientErrors {
    /// client_colour_fragment's (0.2, 0.4, 0.6, 0.8) as a BGRA8 texel's bytes, as Apple's driver stored it
    static let colour: [UInt8] = [0x99, 0x66, 0x33, 0xcc]

    static func state(_ gpu: GPU, _ format: MTLPixelFormat, samples: Int = 1) throws -> MTLRenderPipelineState {
        try gpu.render(
            vertex: "answers_full_vertex", fragment: "client_colour_fragment", format: format, samples: samples
        )
    }

    static func pass(_ t: MTLTexture, resolve: MTLTexture? = nil) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = t
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].resolveTexture = resolve
        pass.colorAttachments[0].storeAction = resolve == nil ? .store : .multisampleResolve
        return pass
    }

    /// Draws one full-target triangle of `state` into `pass`, committed and awaited; the command buffer's status
    static func draw(
        _ gpu: GPU, _ pass: MTLRenderPassDescriptor, _ state: MTLRenderPipelineState, _ work: inout NativeWork,
        _ metrics: inout Metrics
    ) async throws {
        let before = GM20BFacade.counters(of: gpu.device)
        var status = MTLCommandBufferStatus.notEnqueued
        let timing = try await gpu.run(tolerateError: true) { commands in
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(state)
            e.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            e.endEncoding()
            commands.addCompletedHandler { status = $0.status }
        }
        work.paths.append(timing.path)
        record(gpu, before, timing, status, &work, &metrics)
    }

    static func record(
        _ gpu: GPU, _ before: [String: Int], _ timing: GPU.Timing, _ status: MTLCommandBufferStatus,
        _ work: inout NativeWork, _ metrics: inout Metrics
    ) {
        metrics.set("status", status == .completed ? "completed" : "\(status.rawValue)")
        if status != .completed { work.failures.append("the command buffer did not complete") }
        var own = PathTally()
        own.add(timing.facadePath)
        let failed = FailedCounts.judged(
            before: before,
            after: GM20BFacade.counters(of: gpu.device),
            own: own,
            &metrics
        )
        if !failed.1 { work.failures.append(failed.0) }
    }

    /// The BGRA8 texel at (4, 4) of `t`, logged as `field`; a failure where a channel is off `want` by more than `slack`
    static func texel(
        _ gpu: GPU, _ t: MTLTexture, want: [UInt8], slack: Int = 0, field: String, _ work: inout NativeWork,
        _ metrics: inout Metrics
    ) async throws {
        let bytes = try await gpu.read(t, bytesPerPixel: 4)
        let got = Array(bytes.pixel(4, 4, width: t.width))
        metrics.set(field, got.map { String(format: "%02x", $0) }.joined())
        if zip(got, want).contains(where: { abs(Int($0) - Int($1)) > slack }) {
            work.failures.append("\(field) \(got), Apple's \(want)")
        }
    }
}

/// A 4 KiB blit copy from offset 2048 of a 4 KiB buffer into an 8 KiB one: Apple's driver copied the 2048 bytes inside
/// the source to [0, 2048) and left the rest. A fill of 8 KiB from offset 1024 of a 4 KiB buffer filled [1024, 4096).
struct NativeErrorBufferCopyTest: NativeObjectTest {
    let id = "metal.native.error_bufcopy"
    let name = "A buffer copy or fill past its buffer moves the bytes inside it, as Apple's driver, on the driver"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let a = try gpu.buffer(4096), b = try gpu.buffer(8192), c = try gpu.buffer(4096)
        memset(a.contents(), 0xAA, 4096)
        memset(b.contents(), 0x55, 8192)
        memset(c.contents(), 0x11, 4096)
        let before = GM20BFacade.counters(of: gpu.device)
        var status = MTLCommandBufferStatus.notEnqueued
        let timing = try await gpu.run(tolerateError: true) { commands in
            let blit = commands.makeBlitCommandEncoder()!
            blit.copy(from: a, sourceOffset: 2048, to: b, destinationOffset: 0, size: 4096)
            blit.fill(buffer: c, range: 1024 ..< (1024 + 8192), value: 7)
            blit.endEncoding()
            commands.addCompletedHandler { status = $0.status }
        }
        work.paths.append(timing.path)
        ClientErrors.record(gpu, before, timing, status, &work, &metrics)
        let copied = b.contents().bindMemory(to: UInt8.self, capacity: 8192)
        let filled = c.contents().bindMemory(to: UInt8.self, capacity: 4096)
        let copyWrong = (0 ..< 8192).filter { copied[$0] != ($0 < 2048 ? 0xAA : 0x55) }.count
        let fillWrong = (0 ..< 4096).filter { filled[$0] != ($0 < 1024 ? 0x11 : 7) }.count
        metrics.set("copy_wrong", copyWrong)
        metrics.set("fill_wrong", fillWrong)
        if copyWrong != 0 { work.failures.append("\(copyWrong) bytes of the clamped copy wrong") }
        if fillWrong != 0 { work.failures.append("\(fillWrong) bytes of the clamped fill wrong") }
        return work
    }
}

/// drawPrimitives:indirectBuffer: 4 bytes into a 16-byte buffer of {7, 3, 1, 0}: Apple's driver drew a triangle of 3
/// vertices, 1 instance, from the three words inside and a zero after
struct NativeErrorIndexRangeTest: NativeObjectTest {
    let id = "metal.native.error_idxrange"
    let name = "Indirect arguments past their buffer draw from zeros after it, as Apple's driver, on the driver"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let target = try gpu.texture(.bgra8Unorm, 8, 8)
        let state = try ClientErrors.state(gpu, .bgra8Unorm)
        let arguments = try gpu.buffer([UInt32](arrayLiteral: 7, 3, 1, 0))
        work.objects += [target, state]
        let before = GM20BFacade.counters(of: gpu.device)
        var status = MTLCommandBufferStatus.notEnqueued
        let timing = try await gpu.run(tolerateError: true) { commands in
            let e = commands.makeRenderCommandEncoder(descriptor: ClientErrors.pass(target))!
            e.setRenderPipelineState(state)
            e.drawPrimitives(type: .triangle, indirectBuffer: arguments, indirectBufferOffset: 4)
            e.endEncoding()
            commands.addCompletedHandler { status = $0.status }
        }
        work.paths.append(timing.path)
        ClientErrors.record(gpu, before, timing, status, &work, &metrics)
        try await ClientErrors.texel(gpu, target, want: ClientErrors.colour, field: "texel", &work, &metrics)
        return work
    }
}

/// An RGBA8Unorm state drawn into a BGRA8Unorm pass: Apple's driver wrote the right colour, 99 66 33 cc
struct NativeErrorColourFormatTest: NativeObjectTest {
    let id = "metal.native.error_colorfmt"
    let name = "A state of the other channel order draws the right colour into its pass, as Apple's driver, on raw"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let target = try gpu.texture(.bgra8Unorm, 8, 8)
        let state = try ClientErrors.state(gpu, .rgba8Unorm)
        work.objects += [target, state]
        try await ClientErrors.draw(gpu, ClientErrors.pass(target), state, &work, &metrics)
        try await ClientErrors.texel(gpu, target, want: ClientErrors.colour, field: "texel", &work, &metrics)
        return work
    }
}

/// An RGBA16Float state drawn into a BGRA8Unorm pass: Apple's driver wrote its outputs' half-float bits (66 32 66 36),
/// which this GPU has no form of: the draw is skipped and the pass keeps its clear
struct NativeErrorFormatSkipTest: NativeObjectTest {
    let id = "metal.native.error_format_skip"
    let name = "A state whose colour Apple's driver writes as other bits is skipped, its command buffer completed"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let target = try gpu.texture(.bgra8Unorm, 8, 8)
        let state = try ClientErrors.state(gpu, .rgba16Float)
        work.objects += [target, state]
        try await ClientErrors.draw(gpu, ClientErrors.pass(target), state, &work, &metrics)
        try await ClientErrors.texel(gpu, target, want: [0, 0, 0, 0], field: "texel", &work, &metrics)
        return work
    }
}

/// A one-sample state drawn into a four-sample pass: Apple's driver wrote sample 0 alone, resolved 26 1a 0d 33; a
/// four-sample state into a one-sample pass drew the whole colour
struct NativeErrorSamplesTest: NativeObjectTest {
    let id = "metal.native.error_samples"
    let name = "States of other samples than their pass draw as Apple's driver draws them, on the driver"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let ms = try gpu.texture(.bgra8Unorm, 8, 8, samples: 4), resolved = try gpu.texture(.bgra8Unorm, 8, 8)
        let one = try ClientErrors.state(gpu, .bgra8Unorm)
        let target = try gpu.texture(.bgra8Unorm, 8, 8)
        let four = try ClientErrors.state(gpu, .bgra8Unorm, samples: 4)
        work.objects += [ms, resolved, one, target, four]
        try await ClientErrors.draw(gpu, ClientErrors.pass(ms, resolve: resolved), one, &work, &metrics)
        try await ClientErrors.texel(
            gpu, resolved, want: [0x26, 0x1a, 0x0d, 0x33], slack: 1, field: "one_in_four", &work, &metrics
        )
        try await ClientErrors.draw(gpu, ClientErrors.pass(target), four, &work, &metrics)
        try await ClientErrors.texel(gpu, target, want: ClientErrors.colour, field: "four_in_one", &work, &metrics)
        return work
    }
}

/// Four sample positions on a two-sample pass (Metal's setter takes a count of 4): Apple's driver drew the whole colour,
/// resolved 99 66 33 cc
struct NativeErrorPositionsTest: NativeObjectTest {
    let id = "metal.native.error_positions"
    let name = "Sample positions past the pass's samples draw at the standard ones, as Apple's driver, on raw"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let ms = try gpu.texture(.bgra8Unorm, 8, 8, samples: 2), resolved = try gpu.texture(.bgra8Unorm, 8, 8)
        let state = try ClientErrors.state(gpu, .bgra8Unorm, samples: 2)
        work.objects += [ms, resolved, state]
        let pass = ClientErrors.pass(ms, resolve: resolved)
        pass.setSamplePositions([
            MTLSamplePosition(x: 0.25, y: 0.25), MTLSamplePosition(x: 0.75, y: 0.25),
            MTLSamplePosition(x: 0.25, y: 0.75), MTLSamplePosition(x: 0.75, y: 0.75),
        ])
        try await ClientErrors.draw(gpu, pass, state, &work, &metrics)
        try await ClientErrors.texel(
            gpu, resolved, want: ClientErrors.colour, slack: 1, field: "texel", &work, &metrics
        )
        return work
    }
}
