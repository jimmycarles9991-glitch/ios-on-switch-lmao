import Foundation
import Metal

/// Whether the process's GPU still answers after a GPU test timed out: one fill on a fresh queue. A test that hangs
/// keeps waiting on its own queue; when the probe hangs too, every later test that draws would only time out after it.
public enum GPUProbe {
    public static func answers(within seconds: TimeInterval = 5) async -> Bool {
        let waiter = Waiter<Bool>()
        DispatchQueue.global(qos: .userInitiated).async {
            guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(),
                let buffer = device.makeBuffer(length: 64, options: .storageModeShared),
                let commands = queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder()
            else {
                waiter.resume(false)
                return
            }
            blit.fill(buffer: buffer, range: 0..<64, value: 0xA5)
            blit.endEncoding()
            commands.addCompletedHandler { done in
                waiter.resume(done.status == .completed && buffer.contents().load(as: UInt8.self) == 0xA5)
            }
            commands.commit()
        }
        return await waiter.wait(timeout: seconds) ?? false
    }
}

extension HWTest {
    /// Tests that use the app's Metal device, directly, through a CAMetalLayer, Core Image or MPS: the ones a wedged
    /// device would hang. The display tests draw through the render server alone.
    var drawsWithGPU: Bool {
        [.metal, .compositor, .images].contains(subsystem)
    }
}
