import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import UIKit

/// A copy engine reset under the display's present, as a context-switch timeout on the present's channel gets one
/// (debug kernels, drivers/TegraGM20B/TegraGM20BCopyResetABI.h): the kernel resets the copy engine while it copies the
/// next frame the display presents through the GPU. That frame is copied again on the GPU and flipped (its completion
/// carries status 0, so the display took no CPU copy), and the frame after it is copied whole: its scanout read-back
/// equals its framebuffer.
struct PresentCopyResetTest: HWTest {
    let id = "gpu.present.copy_reset"
    let name = "A copy engine reset under the present: the frame is copied again on the GPU and flipped"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 40 }

    static let method: UInt32 = 0x4352
    static let disarm: UInt32 = 0, arm: UInt32 = 1, query: UInt32 = 2
    static let armed: Int32 = 1, inFlight: Int32 = 2, reset: Int32 = 3, done: Int32 = 4, missed: Int32 = 5
    static let attempts = 5

    func run(_ context: TestContext) async throws -> Outcome {
        guard let gpu = GPUConnection() else { return .fail("no TegraGM20B connection") }
        let dot = await context.onStage { stage -> UIView in
            let dot = UIView(frame: CGRect(x: stage.bounds.midX - 40, y: stage.bounds.midY - 40, width: 80, height: 80))
            dot.backgroundColor = .systemTeal
            stage.addSubview(dot)
            return dot
        }
        let nudge: @MainActor () -> Void = { dot.alpha = dot.alpha == 1 ? 0.9 : 1 }
        defer { _ = Self.call(gpu, Self.disarm) }

        var reply: [UInt8] = []
        var tries = 0
        var missed = 0
        while tries < Self.attempts {
            tries += 1
            let (kr, _) = Self.call(gpu, Self.arm)
            if kr == GPUConnection.kIOReturnUnsupported { return .skip("a release kernel has no copy reset") }
            guard kr == KERN_SUCCESS else { return .fail("arm: \(GPUConnection.hex(kr))") }
            reply = await Self.settle(gpu, nudge)
            let state = GPUConnection.int32(reply, 0)
            if state == Self.done || state == Self.armed { break }
            missed += 1
        }
        let state = GPUConnection.int32(reply, 0)
        let status = GPUConnection.int32(reply, 4)
        let redone = Int(GPUConnection.uint32(reply, 12))
        var metrics = Metrics()
        metrics.set("state", Int(state))
        metrics.set("status", Int(status))
        metrics.set("redone", redone)
        metrics.set("engine_status", String(format: "%08x", GPUConnection.uint32(reply, 8)))
        metrics.set("tries", tries)
        metrics.set("missed", missed)
        if state == Self.armed { return .skip("no frame came through a GPU copy (direct scanout)", metrics) }

        var nextWhole = false
        switch await ScanoutReadback.capture(nudge: nudge) {
        case .frame(let frame):
            metrics.set("next_direct", frame.direct)
            metrics.set("next_copy_rows", frame.copyRows)
            nextWhole = frame.direct || frame.copyRows == 0
        case .unavailable(let why):
            metrics.set("next_readback", why)
        }
        return .judged(
            [
                ("the copy engine was reset under the present", state == Self.done),
                ("the cut frame was copied again on the GPU", redone >= 1),
                ("the display flipped it: status 0, no CPU copy", state == Self.done && status == 0),
                ("the next frame was copied whole", nextWhole),
            ], metrics)
    }

    /// Nudges a frame every 100 ms until the armed present is done or missed, for up to 3 s.
    static func settle(_ gpu: GPUConnection, _ nudge: @escaping @MainActor () -> Void) async -> [UInt8] {
        var reply: [UInt8] = []
        for _ in 0..<30 {
            await MainActor.run { nudge() }
            try? await Task.sleep(nanoseconds: 100_000_000)
            let (kr, answer) = call(gpu, query)
            guard kr == KERN_SUCCESS else { return reply }
            reply = answer
            let state = GPUConnection.int32(reply, 0)
            if state == done || state == missed { break }
        }
        return reply
    }

    static func call(_ gpu: GPUConnection, _ op: UInt32) -> (kern_return_t, [UInt8]) {
        gpu.call(method, GPUConnection.words([op, 0]), outputSize: 24)
    }
}
