import CoreVideo
import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal
import VideoToolbox

/// host1x.md A2's producer increments: gpu.x1.gpu_vic_sync's chain (a GPU draw into a pixel buffer, a
/// VTPixelTransferSession scale by VIC, a GPU sample of its output) runs `rounds` times; from the second round on VIC
/// waits on the drawing channel's own syncpoint, with no GPU job of the bridge's. jobless: VIC bridges that waited on a
/// syncpoint of this process's channels (debug kernels, TegraGM20BPathsOpBridge); stale, unwritten, torn: as
/// gpu.x1.gpu_vic_sync's, over every round (0). A run with no jobless round fails: the drawing job did not end with the
/// L2 drain (an Internal job, TegraGM20BJobExternal unset), so every bridge made its own job.
struct X1BridgeJoblessTest: HWTest {
    let id = "gpu.x1.bridge_jobless"
    let name = "VIC reads a GPU drawing it waited for on the drawing channel's own syncpoint"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let rounds = 24
    static let bridgeOp: UInt32 = 8

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        var metrics = Metrics()
        var session: VTPixelTransferSession?
        guard VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session) == noErr, let session
        else { return .fail("VTPixelTransferSessionCreate failed") }
        defer { VTPixelTransferSessionInvalidate(session) }
        let state = try gpu.render(fragment: "x1_spun_colour", format: .bgra8Unorm)
        let connection = GPUConnection()
        guard let before = Self.jobless(connection) else { return .skip("no bridge counts (a release kernel)") }
        var totals = (stale: 0, unwritten: 0, torn: 0)
        for _ in 0 ..< Self.rounds {
            let width = X1GpuVicSyncTest.width, height = X1GpuVicSyncTest.height
            let source = try X1GpuVicSyncTest.buffer(width, height, X1GpuVicSyncTest.stale)
            let destination = try X1GpuVicSyncTest.buffer(width / 2, height / 2, X1GpuVicSyncTest.unwritten)
            let target = try X1GpuVicSyncTest.texture(gpu, source)
            let sampled = try X1GpuVicSyncTest.texture(gpu, destination)
            let draw = try gpu.commit { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .load
                pass.colorAttachments[0].storeAction = .store
                let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
                encoder.setRenderPipelineState(state)
                var corners = GPU.fullScreen
                var seeded: (UInt32, UInt32, Float, UInt32) = (0, X1GpuVicSyncTest.spin, 0, 0)
                var colour = X1GpuVicSyncTest.drawn
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                encoder.setFragmentBytes(&seeded, length: 16, index: 0)
                encoder.setFragmentBytes(&colour, length: 16, index: 1)
                encoder.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                encoder.endEncoding()
            }
            let status = VTPixelTransferSessionTransferImage(session, from: source, to: destination)
            let texels = try await gpu.texels(sampled)
            try gpu.finish([draw])
            guard status == noErr else { return .fail("VTPixelTransferSessionTransferImage: \(status)", metrics) }
            let reading = X1GpuVicSyncTest.classify(texels)
            totals.stale += reading.stale
            totals.unwritten += reading.unwritten
            totals.torn += reading.torn
        }
        let jobless = (Self.jobless(connection) ?? before) - before
        metrics.set("rounds", Self.rounds)
        metrics.set("jobless", jobless)
        metrics.set("stale", totals.stale)
        metrics.set("unwritten", totals.unwritten)
        metrics.set("torn", totals.torn)
        metrics.set("paths", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        if jobless == 0 {
            return .fail(
                "no round's VIC job waited on the drawing channel's own syncpoint: the drawing job did not end with "
                    + "the L2 drain", metrics
            )
        }
        let offRaw = GM20BFacade.kind(of: gpu.device) == "gm20b" ? gpu.paths.offRaw : PathTally()
        return Outcome.judged(
            [
                ("no sample of the source's CPU fill", totals.stale == 0),
                ("no sample of the destination's CPU fill", totals.unwritten == 0),
                ("no torn sample", totals.torn == 0),
            ], metrics, proof: gpu.proof
        ).failing(offRaw: offRaw)
    }

    /// "jobless=N": this process's channels' jobless VIC bridges (TegraGM20BPathsABI.h TegraGM20BPathsOpBridge)
    static func jobless(_ connection: GPUConnection?) -> Int? {
        guard let connection else { return nil }
        let (kr, reply) = connection.call(
            GPUPaths.method, GPUConnection.words([bridgeOp, 0]), outputSize: GPUPaths.replySize
        )
        guard kr == KERN_SUCCESS else { return nil }
        let counts = GPUPaths.parse(String(decoding: reply.prefix { $0 != 0 }, as: UTF8.self)).counts
        return counts["jobless"]
    }
}
