import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Rasterization rate maps are absent on both identities, on purpose (GM20B has no variable-rate unit): the device answers
/// supportsRasterizationRateMapWithLayerCount: NO, makes none (nil), and draws a pass carrying a rate map at full rate with
/// its command buffer completed, as Metal's iOS simulator GPU, which has none, does. The pass's map is made here, since
/// the device makes none.
struct NativeRateMapAbsentTest: NativeObjectTest {
    let id = "metal.native.rate_map_absent"
    let name = "No rate maps: answered NO, none made, a pass carrying one drawn at full rate on the driver"

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        var work = NativeWork()
        let answered = (1 ... 4).filter { gpu.device.supportsRasterizationRateMap(layerCount: $0) }
        metrics.set("supported_layer_counts", answered.isEmpty ? "-" : answered.map(String.init).joined(separator: ","))
        if !answered.isEmpty { work.failures.append("rate maps answered YES for \(answered)") }
        let layer = MTLRasterizationRateLayerDescriptor(sampleCount: MTLSize(width: 2, height: 2, depth: 0))
        let descriptor = MTLRasterizationRateMapDescriptor(
            screenSize: MTLSize(width: 32, height: 32, depth: 0), layer: layer
        )
        let made = gpu.device.makeRasterizationRateMap(descriptor: descriptor)
        metrics.set("made", made.map { NSStringFromClass(type(of: $0 as AnyObject)) } ?? "nil")
        if made != nil { work.failures.append("a rate map was made") }
        let target = try gpu.texture(.bgra8Unorm, 32, 32)
        work.objects.append(target)
        let pass = ClientErrors.pass(target)
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 0.8)
        pass.rasterizationRateMap = ForeignRateMap(device: gpu.device)
        let before = GM20BFacade.counters(of: gpu.device)
        var status = MTLCommandBufferStatus.notEnqueued
        let timing = try await gpu.run(tolerateError: true) { commands in
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
            commands.addCompletedHandler { status = $0.status }
        }
        work.paths.append(timing.path)
        ClientErrors.record(gpu, before, timing, status, &work, &metrics)
        try await ClientErrors.texel(gpu, target, want: ClientErrors.colour, slack: 1, field: "texel", &work, &metrics)
        return work
    }
}

/// A rate map the caller made itself, of one 32x32 layer at full rate
private final class ForeignRateMap: NSObject, MTLRasterizationRateMap {
    let device: MTLDevice
    let label: String? = "foreign"
    let screenSize = MTLSize(width: 32, height: 32, depth: 0)
    let physicalGranularity = MTLSize(width: 32, height: 32, depth: 0)
    let layerCount = 1
    let parameterDataSizeAndAlign = MTLSizeAndAlign(size: 256, align: 4)

    init(device: MTLDevice) {
        self.device = device
    }

    func copyParameterData(buffer: MTLBuffer, offset: Int) {}

    func physicalSize(layer: Int) -> MTLSize {
        screenSize
    }

    func physicalCoordinates(screenCoordinates: MTLCoordinate2D, layer: Int) -> MTLCoordinate2D {
        screenCoordinates
    }

    func screenCoordinates(physicalCoordinates: MTLCoordinate2D, layer: Int) -> MTLCoordinate2D {
        physicalCoordinates
    }
}
