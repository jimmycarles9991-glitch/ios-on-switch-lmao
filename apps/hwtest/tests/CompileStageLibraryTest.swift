import Darwin
import Foundation
import HWTestCore
import HWTestGPU
import MachO
import Metal

/// A render state and a kernel no pack or store holds, made by the shader compiler with NAK's stage compile linked into
/// the plugin (third_party/mesa/src/nouveau/switch_compile): the plugin image exports switch_compile_stage, the state is
/// the facade's, compiled by the compiler rather than in this process, its kernel computes every element, and no
/// command buffer failed. Before the stage compile left NVK, the plugin had no such export and the compiler found
/// NAK's exports in libvulkan_nouveau.dylib.
struct CompileStageLibraryTest: HWTest {
    let id = "gpu.compile.stage_library"
    let name = "States the pack lacks compiled through the stage compile library linked into the plugin"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        guard GM20BFacade.kind(of: gpu.device) == "gm20b" else {
            return .skip("the default device is not the facade")
        }
        var metrics = Metrics()
        let plugin = Self.pluginImage()
        let export = plugin.map { Self.export("switch_compile_stage", in: $0) } ?? "no plugin image"
        metrics.set("plugin", plugin.map { ($0 as NSString).lastPathComponent } ?? "none")
        metrics.set("stage_compile", export)
        metrics.set("nvk_loaded", Self.imageLoaded("libvulkan_nouveau.dylib") ? 1 : 0)

        let before = GM20BFacade.counters(of: gpu.device)
        let salt = UInt32.random(in: 1 ... UInt32.max) ^ 0x6e61_6b6c
        let state = try pipeline(gpu, salt: salt)
        let compile = GM20BFacade.compile(of: state) ?? [:]
        let how = compile["how"] as? String ?? "none"
        let packed = GM20BFacade.packed(state)
        let wrong = try await kernel(gpu, salt: salt)
        let after = GM20BFacade.counters(of: gpu.device)

        metrics.set("how", how)
        metrics.set("packed", packed.map { $0 ? 1 : 0 } ?? -1)
        metrics.set("kernel_wrong", wrong)
        let failed = FailedCounts.judged(before: before, after: after, own: gpu.paths, &metrics)
        return .judged(
            [
                ("the plugin image exports switch_compile_stage", export == "plugin"),
                ("the state is the facade's, made at its creation", packed == false),
                ("compiled by the shader compiler", how == "compiled"),
                ("the salted kernel's every element exact", wrong == 0),
                failed,
            ], metrics, proof: "salt \(salt)"
        )
    }

    func pipeline(_ gpu: GPU, salt: UInt32) throws -> MTLRenderPipelineState {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = try gpu.library.makeFunction(name: "salted_fragment", constantValues: constants)
        descriptor.colorAttachments[0].pixelFormat = .bgra8Unorm
        return try gpu.device.makeRenderPipelineState(descriptor: descriptor)
    }

    func kernel(_ gpu: GPU, salt: UInt32) async throws -> Int {
        var value = salt
        let constants = MTLFunctionConstantValues()
        constants.setConstantValue(&value, type: .uint, index: 0)
        let function = try await gpu.library.makeFunction(name: "salted_kernel", constantValues: constants)
        let state = try await gpu.device.makeComputePipelineState(function: function)
        let count = 1024
        let data = try gpu.buffer(count * 4)
        try await gpu.run { commands in
            let encoder = commands.makeComputeCommandEncoder()!
            encoder.setComputePipelineState(state)
            encoder.setBuffer(data, offset: 0, index: 0)
            encoder.dispatchThreads(
                MTLSize(width: count, height: 1, depth: 1),
                threadsPerThreadgroup: MTLSize(width: 64, height: 1, depth: 1)
            )
            encoder.endEncoding()
        }
        let values = data.contents().bindMemory(to: UInt32.self, capacity: count)
        return (0 ..< count).filter { values[$0] != UInt32($0) ^ salt }.count
    }

    /// The loaded TegraGM20BMetal plugin's path
    static func pluginImage() -> String? {
        for i in 0 ..< _dyld_image_count() {
            guard let name = _dyld_get_image_name(i) else { continue }
            let path = String(cString: name)
            if path.hasSuffix("/TegraGM20BMetal.bundle/TegraGM20BMetal") { return path }
        }
        return nil
    }

    static func imageLoaded(_ suffix: String) -> Bool {
        (0 ..< _dyld_image_count()).contains { i in
            _dyld_get_image_name(i).map { String(cString: $0).hasSuffix("/" + suffix) } ?? false
        }
    }

    /// "plugin" where `symbol` resolves into the plugin image at `path`, else what it resolves into or "none"
    static func export(_ symbol: String, in path: String) -> String {
        guard let handle = dlopen(path, RTLD_NOLOAD | RTLD_LAZY) else { return "plugin not loaded" }
        defer { dlclose(handle) }
        guard let address = dlsym(handle, symbol) else { return "none" }
        var info = Dl_info()
        guard dladdr(address, &info) != 0, let image = info.dli_fname else { return "unknown image" }
        let resolved = String(cString: image)
        if resolved.hasSuffix("/TegraGM20BMetal.bundle/TegraGM20BMetal") { return "plugin" }
        return (resolved as NSString).lastPathComponent
    }
}
