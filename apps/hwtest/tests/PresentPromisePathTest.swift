import Foundation
import HWTestCore
import HWTestGPU
import IOSurface
import Metal
import UIKit

/// Writer promises (drivers/TegraGM20B/SwitchGpuWriterPromise.h): a command buffer that waits for a shared event counts
/// as scheduled before its work reaches the kernel, so its waitUntilScheduled returns at once; the IOSurface it renders
/// into is promised first. The render server's render of a layer showing that surface (drawHierarchy) starts right
/// after, while the event is still unsignalled; the event is signalled 30 ms later, and the deliberately long job runs
/// then. The render must show the job's finished frame, not the surface's earlier CPU fill: the render server's job
/// acquires the promise on the GPU, and the held job's fence push releases it (render_ms: the render's wait, the event's
/// 30 ms and the held job).
struct PresentPromisePathTest: GPUPathTest {
    let id = "gpu.path.present_promise"
    let name = "A render of a surface a held command buffer writes shows that command buffer's finished frame"
    let counters = ["writer_promises"]
    static let size = 64, draws = 400
    static let before: [UInt8] = [0, 0, 255, 255]  // BGRA red, the CPU's fill
    static let drawn: SIMD4<Float> = [0, 1, 0, 1]

    func exercise(_ gpu: GPU, _ context: TestContext, _ metrics: inout Metrics) async throws -> [String] {
        let rowBytes = Self.size * 4
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: Self.size, .height: Self.size, .bytesPerElement: 4, .bytesPerRow: rowBytes,
            .allocSize: rowBytes * Self.size, .pixelFormat: 0x4247_5241,
        ]
        guard PathParts.ownRouteRaw(gpu, part: "the held command buffer", counters: counters, &metrics) else {
            return []
        }
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface creation failed") }
        surface.lock(options: [], seed: nil)
        for at in stride(from: 0, to: rowBytes * Self.size, by: 4) {
            (surface.baseAddress + at).copyMemory(from: Self.before, byteCount: 4)
        }
        surface.unlock(options: [], seed: nil)
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: Self.size, height: Self.size, mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .shared
        guard let target = gpu.device.makeTexture(descriptor: descriptor, iosurface: surface, plane: 0),
            let event = gpu.device.makeSharedEvent(), let commands = gpu.queue.makeCommandBuffer()
        else { throw TestFailure("no surface texture, shared event or command buffer") }
        let pipeline = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let shown = await context.onStage { stage -> UIView in
            let view = UIView(frame: CGRect(x: 0, y: 0, width: 96, height: 96))
            view.layer.contents = surface
            view.layer.contentsGravity = .resize
            stage.addSubview(view)
            return view
        }
        await Snapshots.settle()
        commands.encodeWaitForEvent(event, value: 1)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 1, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(pipeline)
        for _ in 0..<Self.draws { GPU.draw(encoder, corners: GPU.fullScreen, color: Self.drawn) }
        encoder.endEncoding()
        commands.commit()
        commands.waitUntilScheduled()
        let scheduledEarly = commands.status.rawValue < MTLCommandBufferStatus.scheduled.rawValue
        DispatchQueue.global().asyncAfter(deadline: .now() + 0.03) { event.signaledValue = 1 }
        let rendering = DispatchTime.now()
        let render = await MainActor.run { Snapshots.composited(shown) }
        let renderMs = Double(DispatchTime.now().uptimeNanoseconds - rendering.uptimeNanoseconds) / 1e6
        commands.waitUntilCompleted()
        await context.clearStage()
        let centre = render.pixel(render.width / 2, render.height / 2)
        metrics.set("held", scheduledEarly ? 1 : 0)
        metrics.set("render_ms", String(format: "%.1f", renderMs))
        metrics.set(
            "centre", centre.prefix(3).map { String(Int(($0 * render.levels).rounded())) }.joined(separator: ","))
        var failures: [String] = []
        if !scheduledEarly { failures.append("waitUntilScheduled waited for the event: nothing was promised early") }
        if commands.status != .completed {
            failures.append("the held command buffer ended \(commands.status.rawValue)")
        }
        if centre[1] < 0.8 || centre[0] > 0.2 {
            failures.append("the render shows \(centre.prefix(3)), not the held job's green")
        }

        return failures
    }
}
