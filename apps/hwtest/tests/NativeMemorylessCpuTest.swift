import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// A CPU copy of a memoryless texture (drivers/TegraGM20BMetal/raw/runtime/RawMetalTexture.mm cpuReachesNothing): getBytes
/// leaves the caller's bytes and replaceRegion moves nothing, as Apple's driver does with API validation off, with no m2v
/// device asked for a twin.
struct NativeMemorylessCpuTest: HWTest {
    let id = "metal.native.memoryless_cpu"
    let name = "CPU copies of a memoryless texture move nothing, with no m2v"
    let subsystem = Subsystem.metal

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        let needsBefore = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .rgba8Unorm, width: 16, height: 16, mipmapped: false)
        descriptor.storageMode = .memoryless
        descriptor.usage = .renderTarget
        guard let texture = gpu.device.makeTexture(descriptor: descriptor) else { throw TestFailure("no texture") }
        let region = MTLRegionMake2D(0, 0, 16, 16)
        let written = [UInt8](repeating: 0x5a, count: 16 * 16 * 4)
        texture.replace(region: region, mipmapLevel: 0, withBytes: written, bytesPerRow: 64)
        var read = [UInt8](repeating: 0xab, count: written.count)
        texture.getBytes(&read, bytesPerRow: 64, from: region, mipmapLevel: 0)
        let moved = read.filter { $0 != 0xab }.count
        let needsAfter = RawFacade.m2vRecord(of: gpu.device)["needs"] as? [String: Any] ?? [:]
        let asked = Set(needsAfter.keys).subtracting(needsBefore.keys).sorted()
        var metrics = Metrics()
        metrics.set("bytes_moved", moved)
        metrics.set("m2v_needs_asked", asked.isEmpty ? "-" : asked.joined(separator: ","))
        var wrong: [String] = []
        if moved != 0 { wrong.append("moved") }
        if !asked.isEmpty { wrong.append("m2v") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }
}
