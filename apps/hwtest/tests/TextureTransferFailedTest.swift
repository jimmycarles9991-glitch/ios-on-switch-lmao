import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import HWTestKit
import IOSurface
import Metal

/// A copy-engine texture transfer that fails (TegraGM20BMetalProbe.mm switchMetalProbeFailTransfer:, debug builds: the calling
/// thread's next getBytes or replaceRegion the copy engine moves fails as the kernel's refusal of its submission does)
/// over an IOSurface the kernel laid out block-linear at the first GPU write, where no CPU path may take the copy over
/// (TegraGM20BMetalTextureCpu.mm transferTexture): a fresh 256x256 'BGRA' surface each run, its texture cleared to (0.2, 0.4,
/// 0.6, 1). Each arm, its copy and its disarm run on one thread with no suspension point between them. Judged:
/// armed_before 0 (nothing armed), fired_write and fired_read 1 (each arm met its copy), texture_unchanged 1 (the
/// failed replaceRegion of 0xff moved nothing: the region still reads the clear), read_untouched 1 (the failed getBytes
/// left its 0x5a buffer alone), next_write_ok 1 (an unarmed replaceRegion afterwards writes, so no arm is left behind),
/// and the clear read back exactly first. The one gm20b:texture.cpu.unreached line both failures share (once a surface
/// ID) is judged by tools/devicerun per run (refusal.texture-unreached-hwtest); surface_cpu_layout is reported, and a
/// surface the kernel left pitch-linear is red: no failure would reach the line there.
struct TextureTransferFailedTest: HWTest {
    let id = "gpu.gm20b.texture_transfer_failed"
    let name = "A copy-engine transfer that fails moves nothing over a block-linear surface"
    let subsystem = Subsystem.metal

    static let side = 256, row = side * 4, region = 16
    static let selector = NSSelectorFromString("switchMetalProbeFailTransfer:")
    static let clear: [UInt8] = [153, 102, 51, 255]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        guard let facade = gpu.device as? NSObject, facade.responds(to: Self.selector) else {
            return .skip(GM20BFacade.missing("switchMetalProbeFailTransfer:", on: gpu.device))
        }
        let (surface, texture) = try Self.surfaceTexture(gpu.device)
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = texture
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0.2, green: 0.4, blue: 0.6, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        var layout = "-"
        if let connection = GPUConnection() {
            let (kr, answer) = connection.surfaceLayout(surface)
            layout = kr == KERN_SUCCESS ? String(answer) : GPUConnection.hex(kr)
        }
        let arm = { (on: Bool) -> Bool in
            (facade.perform(Self.selector, with: NSNumber(value: on))?.takeUnretainedValue() as? NSNumber)?.boolValue
                ?? false
        }
        let reference = Self.read(texture, fill: 0xa5)
        let armedBefore = arm(false)
        _ = arm(true)
        Self.write(texture, 0xff)
        let firedWrite = arm(false)
        let afterWrite = Self.read(texture, fill: 0xa5)
        _ = arm(true)
        let armedRead = Self.read(texture, fill: 0x5a)
        let firedRead = arm(false)
        let written: [UInt8] = Array(repeating: 0x3c, count: Self.region * Self.region * 4)
        Self.write(texture, 0x3c)
        let nextWrite = Self.read(texture, fill: 0xa5)
        var metrics = Metrics()
        metrics.set("surface_cpu_layout", layout)
        metrics.set("surface_id", Int(IOSurfaceGetID(unsafeBitCast(surface, to: IOSurfaceRef.self))))
        metrics.set("armed_before", armedBefore)
        metrics.set("fired_write", firedWrite)
        metrics.set("fired_read", firedRead)
        metrics.set("texture_unchanged", afterWrite == reference)
        metrics.set("read_untouched", armedRead.allSatisfy { $0 == 0x5a })
        metrics.set("next_write_ok", nextWrite == written)
        let clearOff = stride(from: 0, to: reference.count, by: 4).filter {
            Array(reference[$0 ..< $0 + 4]) != Self.clear
        }.count
        metrics.set("reference_off", clearOff)
        return .judged(
            [
                ("the surface is block-linear after its first GPU write (layout \(layout))", layout == "1"),
                ("the unarmed read gives the clear (\(clearOff) texels off)", clearOff == 0),
                ("nothing was armed before the test", !armedBefore),
                ("the armed replaceRegion met its arm", firedWrite),
                ("the armed getBytes met its arm", firedRead),
                ("the failed replaceRegion moved nothing", afterWrite == reference),
                ("the failed getBytes left its buffer alone", armedRead.allSatisfy { $0 == 0x5a }),
                ("an unarmed replaceRegion afterwards writes", nextWrite == written),
            ], metrics, proof: gpu.proof
        ).onRawPath(gpu)
    }

    static func surfaceTexture(_ device: MTLDevice) throws -> (IOSurface, MTLTexture) {
        let properties: [IOSurfacePropertyKey: Any] = [
            .width: side, .height: side, .bytesPerElement: 4, .bytesPerRow: row, .pixelFormat: UInt32(0x4247_5241),
        ]
        guard let surface = IOSurface(properties: properties) else { throw TestFailure("IOSurface not made") }
        let d = MTLTextureDescriptor.texture2DDescriptor(
            pixelFormat: .bgra8Unorm, width: side, height: side, mipmapped: false
        )
        d.usage = [.renderTarget, .shaderRead]
        d.storageMode = .shared
        guard
            let texture = device.makeTexture(
                descriptor: d, iosurface: unsafeBitCast(surface, to: IOSurfaceRef.self), plane: 0
            )
        else { throw TestFailure("no texture over the IOSurface") }
        return (surface, texture)
    }

    static func read(_ texture: MTLTexture, fill: UInt8) -> [UInt8] {
        var bytes = [UInt8](repeating: fill, count: region * region * 4)
        texture.getBytes(
            &bytes, bytesPerRow: region * 4, from: MTLRegionMake2D(0, 0, region, region), mipmapLevel: 0
        )
        return bytes
    }

    static func write(_ texture: MTLTexture, _ value: UInt8) {
        let bytes = [UInt8](repeating: value, count: region * region * 4)
        texture.replace(
            region: MTLRegionMake2D(0, 0, region, region), mipmapLevel: 0, withBytes: bytes, bytesPerRow: region * 4
        )
    }
}
