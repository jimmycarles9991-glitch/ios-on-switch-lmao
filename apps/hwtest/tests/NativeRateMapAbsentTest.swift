import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// Rasterization rate maps are absent on both identities, on purpose (GM20B has no variable-rate unit): the device answers
/// supportsRasterizationRateMapWithLayerCount: NO, makes none (nil), and draws a pass carrying a rate map at full rate with
/// its command buffer completed, as Metal's iOS simulator GPU, which has none, does. The pass's map is made here, since
/// the device makes none. A state whose argument buffer holds a rate map's data, as Weather's VFX particle functions
/// hold theirs, is made with its reflection, whose member answers as an M4's Metal does: a pointer to no data type, 0
/// bytes, aligned 4.
struct NativeRateMapAbsentTest: NativeObjectTest {
    let id = "metal.native.rate_map_absent"
    let name = "No rate maps: answered NO, none made, a pass carrying one drawn at full rate on the driver"

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct RateMapHeld {
        float scale [[id(0)]];
        constant rasterization_rate_map_data *map [[id(1)]];
    };
    vertex float4 rate_map_held_vertex(uint v [[vertex_id]], constant RateMapHeld &held [[buffer(2)]]) {
        return float4(float(v & 1) * held.scale, float(v >> 1), 0, 1);
    }
    fragment half4 rate_map_fragment() { return half4(1); }
    """

    /// The reflection of the held rate map's member, or why there is none
    static func heldMember(_ gpu: GPU) async throws -> String {
        let library = try await gpu.device.makeLibrary(source: source, options: nil)
        let d = MTLRenderPipelineDescriptor()
        d.vertexFunction = library.makeFunction(name: "rate_map_held_vertex")
        d.fragmentFunction = library.makeFunction(name: "rate_map_fragment")
        d.colorAttachments[0].pixelFormat = .bgra8Unorm
        let (_, reflection) = try await gpu.device.makeRenderPipelineState(descriptor: d, options: .bindingInfo)
        guard let reflection else { return "no reflection" }
        guard let held = reflection.vertexBindings.first(where: { $0.name == "held" }) as? MTLBufferBinding,
              let map = held.bufferStructType?.memberByName("map")
        else { return "no member map" }
        guard map.dataType == .pointer, let pointer = map.pointerType() else {
            return "data type \(map.dataType.rawValue)"
        }
        return "pointer to \(pointer.elementType.rawValue), \(pointer.dataSize) bytes, aligned \(pointer.alignment)"
    }

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
        let member = try await Self.heldMember(gpu)
        metrics.set("held_rate_map_member", member)
        if member != "pointer to 0, 0 bytes, aligned 4" { work.failures.append("held rate map member: \(member)") }
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
