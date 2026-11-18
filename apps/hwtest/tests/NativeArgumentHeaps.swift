import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// metal.native.argument_encoder's heaps: two textures of a shared MTLHeap behind an argument buffer, the heap used
/// whole (useHeap), drawn with the exact colours; what a command buffer of that draw names (the encode profile's uses
/// and use_adds, a debug runtime's) is the same for a heap of 8 textures and one of 512
extension NativeArgumentEncoderTest {
    static let heapTextures = [8, 512]

    /// Where a pass that samples a heap's first and last textures through an argument buffer went, whether each half
    /// drew its texture's colour, and the encode profile's events of its command buffer
    func drawHeapThroughArguments(_ gpu: GPU, textures count: Int) async throws -> (String, [String: Double]) {
        guard let function = gpu.library.makeFunction(name: "argument_textures_fragment"),
            let sampler = gpu.device.makeSamplerState(descriptor: MTLSamplerDescriptor())
        else { return ("no_function", [:]) }
        let heapDescriptor = MTLHeapDescriptor()
        heapDescriptor.storageMode = .shared
        heapDescriptor.size = count * 64 * 1024
        guard let heap = gpu.device.makeHeap(descriptor: heapDescriptor) else { return ("no_heap", [:]) }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 16, height: 16, mipmapped: false)
        descriptor.storageMode = .shared
        descriptor.usage = [.shaderRead]
        let textures = (0..<count).compactMap { _ in heap.makeTexture(descriptor: descriptor) }
        guard textures.count == count else { return ("heap_full", [:]) }
        let colours: [(MTLTexture, [UInt8])] = [
            (textures[0], [255, 0, 0, 255]), (textures[count - 1], [0, 0, 255, 255]),
        ]
        for (texture, texel) in colours {
            let bytes = (0..<256).flatMap { _ in texel }
            texture.replace(region: MTLRegionMake2D(0, 0, 16, 16), mipmapLevel: 0, withBytes: bytes, bytesPerRow: 64)
        }
        let encoder = function.makeArgumentEncoder(bufferIndex: 0)
        let arguments = try gpu.buffer(max(encoder.encodedLength, 16))
        encoder.setArgumentBuffer(arguments, offset: 0)
        encoder.setTexture(textures[0], index: 0)
        encoder.setTexture(textures[count - 1], index: 1)
        encoder.setSamplerState(sampler, index: 2)
        let pipeline = try gpu.render(fragment: "argument_textures_fragment", format: .rgba8Unorm)
        let target = try gpu.texture(.rgba8Unorm, 8, 8)
        var drawn: GPU.Timing?
        var events: [String: Double] = [:]
        for _ in 0..<2 {
            let before = RawFacade.encodeProfile(of: gpu.device)
            drawn = try await gpu.run { commands in
                let render = commands.makeRenderCommandEncoder(descriptor: Self.pass(target))!
                render.useHeap(heap, stages: .fragment)
                Self.cover(render, pipeline, arguments)
            }
            let after = RawFacade.encodeProfile(of: gpu.device)
            events = ["uses", "use_adds"].reduce(into: [:]) { $0[$1] = (after[$1] ?? 0) - (before[$1] ?? 0) }
        }
        let pixels = try await gpu.read(target, bytesPerPixel: 4)
        let exact =
            Array(pixels.pixel(1, 4, width: 8)) == [255, 0, 0, 255]
            && Array(pixels.pixel(6, 4, width: 8)) == [0, 0, 255, 255]
        return ("\(drawn?.path ?? "none")/\(exact ? "exact" : "wrong")", events)
    }

    /// heap_draw_path for each heap, and heap_use_cost: "constant" where both heaps' command buffers named the same,
    /// else the two counts
    func heapUseCost(_ gpu: GPU, _ metrics: inout Metrics, _ failures: inout [String]) async {
        var counts: [[String: Double]] = []
        for count in Self.heapTextures {
            var path = "error"
            do {
                let (made, events) = try await drawHeapThroughArguments(gpu, textures: count)
                path = made
                counts.append(events)
            } catch {
                metrics.set("draw_error", "\(error)".replacingOccurrences(of: " ", with: "_"))
            }
            metrics.set("heap_draw_path_\(count)", path)
            if path != "raw/exact" { failures.append("heap_draw_path_\(count) \(path), not raw/exact") }
        }
        guard counts.count == 2, !counts[0].values.allSatisfy({ $0 == 0 }) else {
            metrics.set("heap_use_cost", "unmeasured")
            if !RawFacade.encodeProfile(of: gpu.device).isEmpty { failures.append("heap_use_cost unmeasured") }
            return
        }
        let same = counts[0] == counts[1]
        let both = "\(counts[0])_\(counts[1])".replacingOccurrences(of: " ", with: "")
        metrics.set("heap_use_cost", same ? "constant" : both)
        if !same { failures.append("a command buffer of a larger heap names more") }
    }
}
