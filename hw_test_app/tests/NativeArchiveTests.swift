import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// Binary archives: the device's own, a list of pipeline keys. A render and a compute state of the app's library (no
/// pack holds them) made under failOnBinaryArchiveMiss against an empty archive are nil with the target driver's code
/// 3 and made once the archive holds them; an archive serialized and opened again holds them still; a file of no
/// archive format is invalidFile; with no option the empty archive changes nothing.
struct NativeBinaryArchiveTest: NativeObjectTest {
    let id = "metal.native.binary_archive"
    let name = "Binary archives are the device's own: Metal's miss, add, serialize, reopen"

    static func render(_ gpu: GPU, _ archive: MTLBinaryArchive) -> MTLRenderPipelineDescriptor {
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = gpu.library.makeFunction(name: "quad_vertex")
        descriptor.fragmentFunction = gpu.library.makeFunction(name: "solid_fragment")
        descriptor.colorAttachments[0].pixelFormat = .rgba8Unorm
        descriptor.binaryArchives = [archive]
        return descriptor
    }

    static func compute(_ gpu: GPU, _ archive: MTLBinaryArchive) -> MTLComputePipelineDescriptor {
        let descriptor = MTLComputePipelineDescriptor()
        descriptor.computeFunction = gpu.library.makeFunction(name: "fma_loop")
        descriptor.binaryArchives = [archive]
        return descriptor
    }

    /// The error a creation under failOnBinaryArchiveMiss answered ("made" where it made the state)
    static func miss(_ make: () throws -> Void) -> String {
        do {
            try make()
            return "made"
        } catch {
            let error = error as NSError
            return "\(error.domain)/\(error.code)"
        }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        let archive = try device.makeBinaryArchive(descriptor: MTLBinaryArchiveDescriptor())
        var work = NativeWork(objects: [archive])
        let render = Self.render(gpu, archive), compute = Self.compute(gpu, archive)
        let missed = [
            Self.miss { _ = try device.makeRenderPipelineState(descriptor: render, options: .failOnBinaryArchiveMiss) },
            Self.miss {
                _ = try device.makeComputePipelineState(descriptor: compute, options: .failOnBinaryArchiveMiss)
            },
        ]
        metrics.set("missed", missed.joined(separator: ","))
        if !missed.allSatisfy({ $0.hasPrefix("AGXMetal") && $0.hasSuffix("/3") }) {
            work.failures.append("a state no archive holds was not Metal's miss")
        }
        if Self.miss({ _ = try device.makeRenderPipelineState(descriptor: render) }) != "made" {
            work.failures.append("a creation without the option failed")
        }
        try archive.addRenderPipelineFunctions(descriptor: render)
        try archive.addComputePipelineFunctions(descriptor: compute)
        let held = [
            Self.miss { _ = try device.makeRenderPipelineState(descriptor: render, options: .failOnBinaryArchiveMiss) },
            Self.miss {
                _ = try device.makeComputePipelineState(descriptor: compute, options: .failOnBinaryArchiveMiss)
            },
        ]
        metrics.set("held", held.joined(separator: ","))
        if held != ["made", "made"] { work.failures.append("a state the archive holds was not made") }
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("metal.native.binary_archive")
        try? FileManager.default.removeItem(at: url)
        try archive.serialize(to: url)
        let reopened = MTLBinaryArchiveDescriptor()
        reopened.url = url
        let again = try device.makeBinaryArchive(descriptor: reopened)
        work.objects.append(again)
        let reread = Self.miss {
            _ = try device.makeRenderPipelineState(
                descriptor: Self.render(gpu, again), options: .failOnBinaryArchiveMiss)
        }
        metrics.set("reopened", reread)
        if reread != "made" { work.failures.append("a serialized archive lost its state") }
        try Data("no archive".utf8).write(to: url)
        let invalid = Self.miss { _ = try device.makeBinaryArchive(descriptor: reopened) }
        metrics.set("invalid", invalid)
        if invalid != "\(MTLBinaryArchiveDomain)/\(MTLBinaryArchiveError.invalidFile.rawValue)" {
            work.failures.append("a file of no archive format was not invalidFile")
        }
        try? FileManager.default.removeItem(at: url)
        return work
    }
}
