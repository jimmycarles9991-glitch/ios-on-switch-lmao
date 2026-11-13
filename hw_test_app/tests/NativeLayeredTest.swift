import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// The identity's layered rendering and viewports: 24B119's A13 (Apple6) renders a pass of several layers and a vertex
/// function's choice of 16 viewports. 21H461's A10X (Apple3) has neither feature (Metal Feature Set Tables: Apple5), though
/// its driver keeps every viewport an encoder sets (metal.native.viewports).
enum Layering {
    static var a13: Bool { ProcessInfo.processInfo.operatingSystemVersion.majorVersion >= 27 }

    static func rgba(_ c: [UInt8]) -> SIMD4<Float> { SIMD4<Float>(c.map { Float($0) / 255 }) }

    static func array(_ gpu: GPU, _ size: Int, layers: Int) throws -> MTLTexture {
        let d = NativeTargetPartsTest.descriptor(.type2DArray, size, size, layers: layers)
        guard let t = gpu.device.makeTexture(descriptor: d) else { throw TestFailure("no 2D array of \(layers)") }
        return t
    }

    static func memoryless(_ gpu: GPU, _ format: MTLPixelFormat, _ size: Int, layers: Int) throws -> MTLTexture {
        let d = MTLTextureDescriptor()
        d.textureType = .type2DArray
        d.pixelFormat = format
        d.width = size
        d.height = size
        d.arrayLength = layers
        d.storageMode = .memoryless
        d.usage = .renderTarget
        guard let t = gpu.device.makeTexture(descriptor: d) else {
            throw TestFailure("no memoryless 2D array of format \(format.rawValue)")
        }
        return t
    }

    /// Texels of every layer of `t` not `want(layer)`
    static func wrong(
        _ gpu: GPU, _ t: MTLTexture, paths: inout [String], _ want: (Int, Int, Int) -> [UInt8]
    ) async throws -> Int {
        var wrong = 0
        for layer in 0..<t.arrayLength {
            let got = try await TextureParts.read(gpu, t, TexturePart(level: 0, slice: layer), paths: &paths)
            for y in 0..<t.height {
                for x in 0..<t.width where Array(got.pixel(x, y, width: t.width)) != want(layer, x, y) {
                    wrong += 1
                }
            }
        }
        return wrong
    }
}

/// A layered pass's clears: every layer of each attachment from its slice on, as many as renderTargetArrayLength,
/// cleared to its attachment's colour; the layers before and past them keep theirs. No draw: what a vertex function
/// chooses is metal.native.layered_vertex's.
struct NativeLayeredClearTest: NativeObjectTest {
    let id = "metal.native.layered_clear"
    let name = "A layered pass clears every layer of every attachment from its slice, and no other"
    var unavailable: String? { Layering.a13 ? nil : "21H461's A10X has no layered rendering" }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let a = try Layering.array(gpu, 32, layers: 6), b = try Layering.array(gpu, 32, layers: 6)
        var work = NativeWork(objects: [a, b])
        let first: [UInt8] = [0, 255, 0, 255], second: [UInt8] = [0, 0, 255, 255]
        let timing = try await gpu.run { commands in
            for t in [a, b] {
                for layer in 0..<6 {
                    commands.makeRenderCommandEncoder(
                        descriptor: TextureParts.pass(t, TexturePart(level: 0, slice: layer), load: .clear))!
                        .endEncoding()
                }
            }
            let pass = MTLRenderPassDescriptor()
            pass.renderTargetArrayLength = 4
            for (j, (t, c)) in [(a, first), (b, second)].enumerated() {
                let att = pass.colorAttachments[j]!
                att.texture = t
                att.slice = 1
                att.loadAction = .clear
                att.storeAction = .store
                let v = c.map { Double($0) / 255 }
                att.clearColor = MTLClearColor(red: v[0], green: v[1], blue: v[2], alpha: v[3])
            }
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        work.paths.append(timing.path)
        for (name, t, c) in [("first", a, first), ("second", b, second)] {
            let wrong = try await Layering.wrong(gpu, t, paths: &work.paths) { layer, _, _ in
                (1...4).contains(layer) ? c : TextureParts.clearColor(TexturePart(level: 0, slice: layer))
            }
            metrics.set("wrong_\(name)", wrong)
            if wrong != 0 { work.failures.append("\(wrong) texels of the \(name) attachment's layers") }
        }
        return work
    }
}

/// A layered pass whose depth and second colour attachments are memoryless 2D arrays, from slice 2: the pass backs them
/// with its four layers and clears the stored attachment's layers 2 to 5, the layers before them keeping theirs.
struct NativeLayeredMemorylessTest: NativeObjectTest {
    let id = "metal.native.layered_memoryless"
    let name = "A layered pass backs its memoryless array attachments with its layers"
    var unavailable: String? { Layering.a13 ? nil : "21H461's A10X has no layered rendering" }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let stored = try Layering.array(gpu, 32, layers: 6)
        let color = try Layering.memoryless(gpu, .rgba8Unorm, 32, layers: 6)
        let depth = try Layering.memoryless(gpu, .depth32Float, 32, layers: 6)
        var work = NativeWork(objects: [stored, color, depth])
        let fill: [UInt8] = [255, 0, 255, 255]
        let timing = try await gpu.run { commands in
            for layer in 0..<6 {
                commands.makeRenderCommandEncoder(
                    descriptor: TextureParts.pass(stored, TexturePart(level: 0, slice: layer), load: .clear))!
                    .endEncoding()
            }
            let pass = MTLRenderPassDescriptor()
            pass.renderTargetArrayLength = 4
            for (j, t) in [stored, color].enumerated() {
                let att = pass.colorAttachments[j]!
                att.texture = t
                att.slice = 2
                att.loadAction = .clear
                att.storeAction = j == 0 ? .store : .dontCare
                att.clearColor = MTLClearColor(red: 1, green: 0, blue: 1, alpha: 1)
            }
            pass.depthAttachment.texture = depth
            pass.depthAttachment.slice = 2
            pass.depthAttachment.loadAction = .clear
            pass.depthAttachment.storeAction = .dontCare
            pass.depthAttachment.clearDepth = 1
            commands.makeRenderCommandEncoder(descriptor: pass)!.endEncoding()
        }
        work.paths.append(timing.path)
        let wrong = try await Layering.wrong(gpu, stored, paths: &work.paths) { layer, _, _ in
            layer >= 2 ? fill : TextureParts.clearColor(TexturePart(level: 0, slice: layer))
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels of the stored attachment's layers") }
        return work
    }
}

/// A vertex function's [[render_target_array_index]]: instance i of a quad drawn into layer i of a layered pass, in a
/// colour of its own, over a clear of every layer. Every layer holds its instance's quad and no other.
struct NativeLayeredVertexTest: NativeObjectTest {
    let id = "metal.native.layered_vertex"
    let name = "A vertex function chooses the layer of a layered pass, every layer its own instance's"
    var unavailable: String? { Layering.a13 ? nil : "21H461's A10X has no layered rendering" }

    static func color(_ i: Int) -> [UInt8] { [UInt8(40 * i + 30), 255 - UInt8(30 * i), 64, 255] }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let t = try Layering.array(gpu, 32, layers: 4)
        let pipeline = try gpu.render(vertex: "layered_vertex", fragment: "flat_color_fragment", format: .rgba8Unorm)
        var work = NativeWork(objects: [t])
        let clear: [UInt8] = [0, 0, 0, 255]
        let timing = try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.renderTargetArrayLength = 4
            pass.colorAttachments[0].texture = t
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            e.setRenderPipelineState(pipeline)
            var corners = GPU.pixelQuad(8, 8, 24, 24, width: 32, height: 32)
            var colors = (0..<4).map { Layering.rgba(Self.color($0)) }
            e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
            e.setVertexBytes(&colors, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 1)
            e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4, instanceCount: 4)
            e.endEncoding()
        }
        work.paths.append(timing.path)
        let wrong = try await Layering.wrong(gpu, t, paths: &work.paths) { layer, x, y in
            (8..<24).contains(x) && (8..<24).contains(y) ? Self.color(layer) : clear
        }
        metrics.set("wrong_texels", wrong)
        if wrong != 0 { work.failures.append("\(wrong) texels miss their layer's instance") }
        return work
    }
}
