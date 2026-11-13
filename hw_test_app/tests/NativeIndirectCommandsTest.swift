import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// An indirect command buffer of four draws that inherit their pipeline and buffers, encoded once on the CPU: a draw
/// of the first quarter, an indexed draw of the second, a third reset to nothing, an instanced draw of the fourth.
/// Executed whole in one pass and the second alone in another, by reference to the commands' words; both command
/// buffers stay raw. Then, in one command buffer, a blit encoder resets the first command and copies the fourth over
/// the third on the GPU (and optimizes the range, which changes nothing), and a pass executes them all after it. Last,
/// the range is in a private buffer a blit copy wrote in the same command buffer ({1, 2}: the second and third
/// commands), which the GPU reads; the CPU never does. A descriptor of commands that set their own pipeline has no form
/// here (its creation is a metric).
struct NativeIndirectCommandsTest: NativeObjectTest {
    let id = "metal.native.indirect_commands"
    let name = "Indirect command buffers of inherited state: encoded once, executed by range on the raw path"

    static let size = 64

    /// Four quads, each a quarter of the target's columns, as quad_vertex reads them (four corners each)
    static var quarters: [SIMD4<Float>] {
        (0..<4).flatMap { q in GPU.pixelQuad(q * size / 4, 0, (q + 1) * size / 4, size, width: size, height: size) }
    }

    static func pass(_ target: MTLTexture) -> MTLRenderPassDescriptor {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
        pass.colorAttachments[0].storeAction = .store
        return pass
    }

    /// Columns whose pixels are not green where `drawn` holds their quarter, black elsewhere
    static func wrong(_ pixels: [UInt8], drawn: Set<Int>) -> Int {
        (0..<size * size).filter { i in
            let quarter = (i % size) / (size / 4)
            let want: [UInt8] = drawn.contains(quarter) ? [0, 255, 0, 255] : [0, 0, 0, 255]
            return Array(pixels.pixel(i % size, i / size, width: size)) != want
        }.count
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = [.draw, .drawIndexed]
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard let commands = device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 4, options: [])
        else { throw TestFailure("makeIndirectCommandBuffer returned nil for inherited draws") }
        var work = NativeWork(objects: [commands])
        let indices = try gpu.buffer([UInt16](arrayLiteral: 4, 5, 6, 7))
        commands.indirectRenderCommandAt(0).drawPrimitives(
            .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 1, baseInstance: 0)
        commands.indirectRenderCommandAt(1).drawIndexedPrimitives(
            .triangleStrip, indexCount: 4, indexType: .uint16, indexBuffer: indices, indexBufferOffset: 0,
            instanceCount: 1, baseVertex: 0, baseInstance: 0)
        commands.indirectRenderCommandAt(2).drawPrimitives(
            .triangleStrip, vertexStart: 8, vertexCount: 4, instanceCount: 1, baseInstance: 0)
        commands.indirectRenderCommandAt(2).reset()
        commands.indirectRenderCommandAt(3).drawPrimitives(
            .triangleStrip, vertexStart: 12, vertexCount: 4, instanceCount: 2, baseInstance: 0)

        let pipeline = try gpu.render(fragment: "solid_fragment", format: .rgba8Unorm) {
            $0.supportIndirectCommandBuffers = true
        }
        let whole = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let second = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let edited = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        let ranged = try gpu.texture(.rgba8Unorm, Self.size, Self.size)
        for (target, range, edit) in [(whole, 0..<4, false), (second, 1..<2, false), (edited, 0..<4, true)] {
            let timing = try await gpu.run { buffer in
                if edit {
                    let blit = buffer.makeBlitCommandEncoder()!
                    blit.resetCommandsInBuffer(commands, range: 0..<1)
                    blit.copyIndirectCommandBuffer(
                        commands, sourceRange: 3..<4, destination: commands, destinationIndex: 2)
                    blit.optimizeIndirectCommandBuffer(commands, range: 0..<4)
                    blit.endEncoding()
                }
                let encoder = buffer.makeRenderCommandEncoder(descriptor: Self.pass(target))!
                encoder.setRenderPipelineState(pipeline)
                var corners = Self.quarters
                encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
                var green = SIMD4<Float>(0, 1, 0, 1)
                encoder.setFragmentBytes(&green, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
                encoder.useResource(indices, usage: .read, stages: .vertex)
                encoder.executeCommandsInBuffer(commands, range: range)
                encoder.endEncoding()
            }
            work.paths.append(timing.path)
        }
        let fresh = try device.makeIndirectCommandBuffer(descriptor: descriptor, maxCommandCount: 4, options: [])!
        work.objects.append(fresh)
        for q in 0..<4 {
            fresh.indirectRenderCommandAt(q).drawPrimitives(
                .triangleStrip, vertexStart: 4 * q, vertexCount: 4, instanceCount: 1, baseInstance: 0)
        }
        let staged = try gpu.buffer([UInt32](arrayLiteral: 1, 2))
        guard let rangeBuffer = device.makeBuffer(length: 8, options: .storageModePrivate) else {
            throw TestFailure("no private range buffer")
        }
        let gpuRange = try await gpu.run { buffer in
            let blit = buffer.makeBlitCommandEncoder()!
            blit.copy(from: staged, sourceOffset: 0, to: rangeBuffer, destinationOffset: 0, size: 8)
            blit.endEncoding()
            let encoder = buffer.makeRenderCommandEncoder(descriptor: Self.pass(ranged))!
            encoder.setRenderPipelineState(pipeline)
            var corners = Self.quarters
            encoder.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * corners.count, index: 0)
            var green = SIMD4<Float>(0, 1, 0, 1)
            encoder.setFragmentBytes(&green, length: MemoryLayout<SIMD4<Float>>.stride, index: 0)
            encoder.executeCommandsInBuffer(fresh, indirectBuffer: rangeBuffer, offset: 0)
            encoder.endEncoding()
        }
        work.paths.append(gpuRange.path)
        let wrongRanged = Self.wrong(try await gpu.read(ranged, bytesPerPixel: 4), drawn: [1, 2])
        metrics.set("wrong_gpu_range", wrongRanged)
        if wrongRanged != 0 { work.failures.append("\(wrongRanged) pixels wrong after the range the GPU wrote") }
        let wrongWhole = Self.wrong(try await gpu.read(whole, bytesPerPixel: 4), drawn: [0, 1, 3])
        let wrongSecond = Self.wrong(try await gpu.read(second, bytesPerPixel: 4), drawn: [1])
        let wrongEdited = Self.wrong(try await gpu.read(edited, bytesPerPixel: 4), drawn: [1, 3])
        metrics.set("wrong_whole", wrongWhole)
        metrics.set("wrong_second", wrongSecond)
        metrics.set("wrong_edited", wrongEdited)
        if wrongWhole != 0 { work.failures.append("\(wrongWhole) pixels wrong after executing every command") }
        if wrongSecond != 0 { work.failures.append("\(wrongSecond) pixels wrong after executing the second alone") }
        if wrongEdited != 0 { work.failures.append("\(wrongEdited) pixels wrong after the GPU's reset and copy") }

        let own = MTLIndirectCommandBufferDescriptor()
        own.commandTypes = [.draw]
        own.inheritPipelineState = false
        own.inheritBuffers = false
        own.maxVertexBufferBindCount = 1
        own.maxFragmentBufferBindCount = 1
        let made = device.makeIndirectCommandBuffer(descriptor: own, maxCommandCount: 1, options: []) != nil
        metrics.set("own_state", made ? "made" : "nil")
        return work
    }
}
