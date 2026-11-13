import Foundation
import HWTestCore
import Metal

/// MTLPipelineOptionFailOnBinaryArchiveMiss as Metal answers it: a creation that lists binary archives none of which
/// holds its state is nil with an error, and nothing is compiled for it; without the option the same descriptor is
/// made; once added to the archive it is a hit. A render state (made at once and, on the Switch, through a completion
/// handler) and a compute state of this app's library, against an empty archive and against QuartzCore's
/// default.metallib opened as an archive, which declares Core Animation's states and none of this app's. On the raw
/// facade every archive is the device's own (RawMetalBinaryArchive.mm): the empty one holds what was added to it, and
/// QuartzCore's what the raw pack's archive key lists declare for that file; a miss is the target driver's error domain
/// with Metal's code 3.
struct ArchiveMissScene: GPUScene {
    let id = "gpu.raw.archive_miss"
    let name = "A binary archive miss under FailOnBinaryArchiveMiss is Metal's nil"

    static let quartzCoreLibraries = [
        "/System/Library/Frameworks/QuartzCore.framework/default.metallib",
        "/System/Library/Frameworks/QuartzCore.framework/Versions/A/Resources/default.metallib",
    ]

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let render = MTLRenderPipelineDescriptor()
        render.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        render.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        render.colorAttachments[0].pixelFormat = .bgra8Unorm
        let compute = MTLComputePipelineDescriptor()
        compute.computeFunction = gpu.library.makeFunction(name: "multiply_add")

        let empty = try gpu.device.makeBinaryArchive(descriptor: MTLBinaryArchiveDescriptor())
        render.binaryArchives = [empty]
        compute.binaryArchives = [empty]
        expectMiss(&result, "render, empty archive") { try makeRender(gpu, render) }
        expectMiss(&result, "compute, empty archive") { try makeCompute(gpu, compute) }
        // Not on a Mac: AGXMetalG16X crashed the host test in this call (11-11, a bad capture in _Block_object_assign
        // under createFragmentProgramVariant), which the same calls from Objective-C did not
        if RawFacade.kind(of: gpu.device) != "apple" {
            let handled = await renderThroughHandler(gpu, render)
            result.expect(handled.state == nil && handled.error != nil, "render handler, empty archive: not a miss")
        }
        result.expect((try? makeRender(gpu, render, options: [])) != nil, "render without the option: not made")
        result.expect((try? makeCompute(gpu, compute, options: [])) != nil, "compute without the option: not made")
        try empty.addRenderPipelineFunctions(descriptor: render)
        try empty.addComputePipelineFunctions(descriptor: compute)
        result.expect((try? makeRender(gpu, render)) != nil, "render added to the archive: not a hit")
        result.expect((try? makeCompute(gpu, compute)) != nil, "compute added to the archive: not a hit")

        guard let path = Self.quartzCoreLibraries.first(where: { FileManager.default.fileExists(atPath: $0) }) else {
            result.notes.append("no QuartzCore default.metallib: its archive's miss is not checked")
            return result
        }
        let archiveDescriptor = MTLBinaryArchiveDescriptor()
        archiveDescriptor.url = URL(fileURLWithPath: path)
        let kind = RawFacade.kind(of: gpu.device)
        guard let quartzCore = try? gpu.device.makeBinaryArchive(descriptor: archiveDescriptor) else {
            // This Mac's Metal calls its own QuartzCore library an invalid archive; iOS's (an AIR slice beside Apple GPU
            // binaries) opens on an M4 as an archive holding nothing for it (metal2vulkan-runtime container.rs)
            result.expect(kind == "apple", "QuartzCore's default.metallib did not open as an archive")
            return result
        }
        render.binaryArchives = [quartzCore]
        compute.binaryArchives = [quartzCore]
        let makers: [(String, () throws -> AnyObject)] = [
            ("render", { try makeRender(gpu, render) }), ("compute", { try makeCompute(gpu, compute) }),
        ]
        for (label, make) in makers {
            let error = expectMiss(&result, "\(label), QuartzCore's archive", make)
            if kind != "m2v", let error {
                result.expect(error.code == 3, "\(label), QuartzCore's archive: code \(error.code), not Metal's 3")
            }
            result.metrics.set("\(label)_domain", error?.domain ?? "-")
        }
        return result
    }

    private func makeRender(
        _ gpu: GPU, _ descriptor: MTLRenderPipelineDescriptor, options: MTLPipelineOption = [.failOnBinaryArchiveMiss]
    ) throws -> AnyObject {
        try gpu.device.makeRenderPipelineState(descriptor: descriptor, options: options).0 as AnyObject
    }

    private func makeCompute(
        _ gpu: GPU, _ descriptor: MTLComputePipelineDescriptor, options: MTLPipelineOption = [.failOnBinaryArchiveMiss]
    ) throws -> AnyObject {
        try gpu.device.makeComputePipelineState(descriptor: descriptor, options: options, reflection: nil) as AnyObject
    }

    @discardableResult
    private func expectMiss(_ result: inout SceneResult, _ label: String, _ make: () throws -> AnyObject) -> NSError? {
        do {
            _ = try make()
            result.expect(false, "\(label): made, not a miss")
            return nil
        } catch {
            return error as NSError
        }
    }

    private func renderThroughHandler(
        _ gpu: GPU, _ descriptor: MTLRenderPipelineDescriptor
    ) async -> (
        state: MTLRenderPipelineState?, error: Error?
    ) {
        await withCheckedContinuation { done in
            gpu.device.makeRenderPipelineState(descriptor: descriptor, options: [.failOnBinaryArchiveMiss]) {
                state, _, error in
                done.resume(returning: (state, error))
            }
        }
    }
}
