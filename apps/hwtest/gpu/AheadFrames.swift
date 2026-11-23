import Foundation
import HWTestCore
import Metal

/// Frames recorded at a render thread's pace, so the facade's submit worker encodes each ended pass ahead of the
/// commit (TegraGM20BMetalPipeline.mm): 15 passes over four 64x64 RGBA8 targets, a millisecond apart. Pass p clears target
/// p % 4 (p < 4) to frame f's clear colour or loads it, and draws eight 8x16 cells of row p / 4, cell d from column 8d,
/// flat (17p, 30d + 10, 60f + 30, 255) / 255, each a vertex and a fragment setBytes and a draw, so every pass's tape
/// reaches a hand-off. A frame may fail as the host test facade_ahead_failures_test.cpp makes it fail.
public final class AheadFrames {
    public enum Failure: Sendable {
        case none
        /// The last pass's encoder draws once more after a blit encoder began: the commit fails the command buffer
        case drawInBlit
        /// Pass `at`'s encoder ends a second time: the command buffer fails as it records
        case endedTwice(at: Int)
    }

    public struct Reading: Sendable {
        /// completed or error, the error's MTLCommandBufferError code, the facade's path
        public var status = "completed", code = 0, path = "-"
        /// switchMetalPipelined: encoders, pipelined, abandons, veto, given_back (-1: not answered)
        public var pipelined: [String: Int] = [:]
        /// pipeline_handoffs counted from the failure until the commit (endedTwice; -1 elsewhere or unanswered)
        public var handoffsAfterFailure = -1
        /// Pixels of the four targets more than one code from the model (completed frames)
        public var pixelsOff = 0
    }

    public static let passes = 15, targets = 4, size = 64, cells = 8, cellWidth = 8, cellHeight = 16
    public static let paceMicroseconds: UInt32 = 1000

    let gpu: GPU
    let state: MTLRenderPipelineState
    let into: [MTLTexture]

    public init(_ gpu: GPU) throws {
        self.gpu = gpu
        state = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm)
        into = try (0 ..< Self.targets).map { _ in try gpu.texture(.rgba8Unorm, Self.size, Self.size) }
    }

    static func clearBytes(_ frame: Int) -> [UInt8] { [16, 32, UInt8(48 + 8 * frame), 255] }

    static func cellBytes(_ frame: Int, _ pass: Int, _ cell: Int) -> [UInt8] {
        [UInt8(17 * pass), UInt8(30 * cell + 10), UInt8((60 * frame + 30) & 255), 255]
    }

    static func colour(_ bytes: [UInt8]) -> SIMD4<Float> { SIMD4(bytes.map { Float($0) / 255 }) }

    /// Target `t`'s bytes after frame `frame`: its clear, then every pass into it in order
    static func model(_ frame: Int, target t: Int) -> [UInt8] {
        var bytes = [UInt8]()
        for _ in 0 ..< (size * size) { bytes += clearBytes(frame) }
        for pass in stride(from: t, to: passes, by: targets) {
            for cell in 0 ..< cells {
                let texel = cellBytes(frame, pass, cell)
                for y in (pass / targets) * cellHeight ..< (pass / targets + 1) * cellHeight {
                    for x in cell * cellWidth ..< (cell + 1) * cellWidth {
                        bytes.replaceSubrange((y * size + x) * 4 ..< (y * size + x) * 4 + 4, with: texel)
                    }
                }
            }
        }
        return bytes
    }

    func record(_ commands: MTLCommandBuffer, _ frame: Int, _ failure: Failure, handoffsAt: inout Int?) {
        for pass in 0 ..< Self.passes {
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = into[pass % Self.targets]
            descriptor.colorAttachments[0].loadAction = pass < Self.targets ? .clear : .load
            let clear = Self.colour(Self.clearBytes(frame))
            descriptor.colorAttachments[0].clearColor = MTLClearColor(
                red: Double(clear.x), green: Double(clear.y), blue: Double(clear.z), alpha: Double(clear.w)
            )
            descriptor.colorAttachments[0].storeAction = .store
            guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return }
            encoder.setRenderPipelineState(state)
            let row = pass / Self.targets * Self.cellHeight
            for cell in 0 ..< Self.cells {
                let x = cell * Self.cellWidth
                GPU.draw(
                    encoder,
                    corners: GPU.pixelQuad(
                        x, row, x + Self.cellWidth, row + Self.cellHeight, width: Self.size, height: Self.size
                    ),
                    color: Self.colour(Self.cellBytes(frame, pass, cell))
                )
            }
            encoder.endEncoding()
            if case .endedTwice(let at) = failure, at == pass {
                encoder.endEncoding()
                handoffsAt = GM20BFacade.counters(of: gpu.device)["pipeline_handoffs"]
            }
            if case .drawInBlit = failure, pass == Self.passes - 1, let blit = commands.makeBlitCommandEncoder() {
                GPU.draw(
                    encoder, corners: GPU.pixelQuad(0, 0, Self.size, Self.size, width: Self.size, height: Self.size),
                    color: SIMD4(1, 1, 1, 1)
                )
                blit.endEncoding()
            }
            usleep(Self.paceMicroseconds)
        }
    }

    /// Frame `frame` recorded with `failure`, committed and waited for
    public func run(_ frame: Int, _ failure: Failure) async throws -> Reading {
        var buffer: MTLCommandBuffer?
        var handoffsAt: Int?
        var handoffsBeforeCommit: Int?
        let timing = try await gpu.run(tolerateError: true) { commands in
            buffer = commands
            record(commands, frame, failure, handoffsAt: &handoffsAt)
            handoffsBeforeCommit = GM20BFacade.counters(of: gpu.device)["pipeline_handoffs"]
        }
        var reading = Reading(path: timing.facadePath)
        if timing.path.hasPrefix("error:") {
            reading.status = "error"
            reading.code = (buffer?.error as NSError?)?.code ?? -1
        }
        if let buffer { reading.pipelined = GM20BFacade.pipelined(buffer).mapValues(\.intValue) }
        if let at = handoffsAt, let commit = handoffsBeforeCommit { reading.handoffsAfterFailure = commit - at }
        if reading.status == "completed" {
            for t in 0 ..< Self.targets {
                let got = try await gpu.read(into[t], bytesPerPixel: 4), want = Self.model(frame, target: t)
                reading.pixelsOff += stride(from: 0, to: got.count, by: 4).filter { at in
                    (0 ..< 4).contains { abs(Int(got[at + $0]) - Int(want[at + $0])) > 1 }
                }.count
            }
        }
        return reading
    }
}
