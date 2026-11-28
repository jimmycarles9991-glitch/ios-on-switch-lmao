import Foundation
import HWTestCore
import HWTestGPU
import HWTestKit
import Metal

/// Destination-reading draws, each scene (FetchShapes.swift) drawn into each FetchTarget cleared to 0; its fragment adds
/// one red code to the colour it read, so a pixel's red counts the fragments that reached it in order. On both targets
/// each call is drawn whole inside the pixel interlock (their storage forms, the translator's fetch/packed.rs). Each test
/// proves one shape there: the primitive count, a long list, strip restart and winding, points and lines, primitive_id,
/// instance_id, the absolute vertex_id, the indirect forms, and the draws of an indirect command buffer; and that each
/// call was one call's words on the CPU (FetchRoute). The fetch macro's route (a state with no storage form) is not
/// drawn here. Fields per target (_b5g6r5,
/// _bgr5a1): wrong= pixels whose codes are not the scene's (first= the first, scene:x,y:red/green/blue against the
/// scene's), interlocked=, sequential= and triangles= the facade's fetch counters over its scenes; path=; failed_cbs=.
struct FetchShapeTest: HWTest {
    let id: String
    let name: String
    let shapes: [FetchShape]
    let subsystem = Subsystem.metal

    static let all: [any HWTest] = [
        FetchShapeTest(
            id: "gpu.gm20b.fetch_list", name: "A fetching list's triangles each read the last", shapes: [.list]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_long", name: "A fetching list of 128 triangles, past the old 96-triangle cap",
            shapes: [.long]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_strip_restart",
            name: "A fetching strip keeps its winding across primitives, and an indexed one across restarts",
            shapes: [.stripRestart, .stripRun]
        ),
        FetchShapeTest(id: "gpu.gm20b.fetch_points", name: "Fetching points each read the last", shapes: [.points]),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_lines", name: "Fetching lines and a line strip each read the last", shapes: [.lines]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_primitive_id", name: "A fetching draw's primitive_id counts across its primitives",
            shapes: [.primitiveID]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_instances", name: "A fetching draw's instances each draw every primitive, in order",
            shapes: [.instances]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_vertex_id", name: "A fetching draw's vertex_id counts from its vertexStart",
            shapes: [.vertexID]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_indirect", name: "Indirect fetching draws, a list and a restarted strip",
            shapes: [.indirectList, .indirectStrip]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_icb", name: "An indirect command buffer's fetching draws: a list, a strip and points",
            shapes: [.icbDraws]
        ),
        FetchShapeTest(
            id: "gpu.gm20b.fetch_icb_indexed",
            name: "An indirect command buffer's indexed fetching draws: a list, a strip and points",
            shapes: [.icbIndexedDraws]
        ),
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = GM20BFacade.kind(of: gpu.device)
        guard kind == "gm20b" else { return .skip("the default device is not the facade (\(kind))") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        let start = GM20BFacade.counters(of: gpu.device)
        let calls = shapes.map { $0.icb ? 1 : $0.draws.count }.reduce(0, +)
        let triangles = shapes.filter { !$0.icb }.flatMap(\.draws).map(\.counted).reduce(0, +)
        for target in FetchTarget.all {
            let before = GM20BFacade.counters(of: gpu.device)
            var wrong = 0
            var first = "-"
            do {
                for (index, shape) in shapes.enumerated() {
                    let texture = try gpu.texture(target.format, shape.side, shape.side, usage: [.renderTarget])
                    let state = try gpu.render(vertex: shape.vertex, fragment: shape.fragment, format: target.format) {
                        $0.supportIndirectCommandBuffers = shape.icb
                    }
                    let path = try await gpu.run(tolerateError: true) {
                        try Self.encode(shape, state, into: texture, gpu, $0)
                    }.path
                    checks.append(("\(target.name) scene \(index): its command buffer on the driver", path == "gm20b"))
                    let got = target.codes(try await gpu.read(texture, bytesPerPixel: 2))
                    let off = zip(got, target.expected(shape.codes)).enumerated()
                        .filter { $0.element.0 != $0.element.1 }
                    wrong += off.count
                    if let o = off.first, first == "-" {
                        let (x, y) = (o.offset % shape.side, o.offset / shape.side)
                        first = "\(index):\(x),\(y):\(Self.shown(o.element.0))_want_\(Self.shown(o.element.1))"
                    }
                }
            } catch {
                checks.append(("\(target.name): \(GPU.describe(error))", false))
                continue
            }
            let after = GM20BFacade.counters(of: gpu.device)
            var moved: [String: Int] = [:]
            for key in FetchRoute.keys {
                moved[key] = (after[key] ?? 0) - (before[key] ?? 0)
                metrics.set("\(key.dropFirst("fetch_".count))_\(target.name)", moved[key] ?? 0)
            }
            metrics.set("wrong_\(target.name)", wrong)
            metrics.set("first_\(target.name)", first)
            checks.append(("\(target.name): every pixel at the scene's codes", wrong == 0))
            checks += target.route.checks(calls: calls, triangles: triangles, moved: moved)
        }
        let end = GM20BFacade.counters(of: gpu.device)
        metrics.set("path", gpu.paths.isEmpty ? "-" : gpu.paths.text)
        metrics.merge(PathTally.counterMetrics(before: start, after: end))
        checks.append(FailedCounts.judged(before: start, after: end, own: gpu.paths, &metrics))
        return .judged(checks, metrics, proof: gpu.proof)
    }

    static func encode(
        _ shape: FetchShape, _ state: MTLRenderPipelineState, into target: MTLTexture, _ gpu: GPU,
        _ commands: MTLCommandBuffer
    ) throws {
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        pass.colorAttachments[0].storeAction = .store
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(state)
        encoder.setCullMode(shape.cull)
        if shape.icb {
            try indirectCommands(shape, encoder, gpu)
            encoder.endEncoding()
            return
        }
        for draw in shape.draws {
            encoder.setVertexBuffer(try gpu.buffer(draw.points), offset: 0, index: 0)
            switch (draw.indices, draw.indirect) {
            case (nil, false):
                encoder.drawPrimitives(
                    type: draw.primitive, vertexStart: draw.vertexStart, vertexCount: draw.count,
                    instanceCount: draw.instances
                )
            case (let indices?, false):
                encoder.drawIndexedPrimitives(
                    type: draw.primitive, indexCount: draw.count, indexType: .uint16,
                    indexBuffer: try gpu.buffer(indices), indexBufferOffset: 0, instanceCount: draw.instances
                )
            case (nil, true):
                let arguments = MTLDrawPrimitivesIndirectArguments(
                    vertexCount: UInt32(draw.count), instanceCount: UInt32(draw.instances),
                    vertexStart: UInt32(draw.vertexStart), baseInstance: 0
                )
                encoder.drawPrimitives(
                    type: draw.primitive, indirectBuffer: try gpu.buffer([arguments]), indirectBufferOffset: 0
                )
            case (let indices?, true):
                let arguments = MTLDrawIndexedPrimitivesIndirectArguments(
                    indexCount: UInt32(draw.count), instanceCount: UInt32(draw.instances), indexStart: 0,
                    baseVertex: 0, baseInstance: 0
                )
                encoder.drawIndexedPrimitives(
                    type: draw.primitive, indexType: .uint16, indexBuffer: try gpu.buffer(indices),
                    indexBufferOffset: 0, indirectBuffer: try gpu.buffer([arguments]), indirectBufferOffset: 0
                )
            }
        }
        encoder.endEncoding()
    }

    /// The scene's draws as one indirect command buffer's commands, run in the encoder's pass
    static func indirectCommands(_ shape: FetchShape, _ encoder: MTLRenderCommandEncoder, _ gpu: GPU) throws {
        let indexed = shape.draws.contains { $0.indices != nil }
        let descriptor = MTLIndirectCommandBufferDescriptor()
        descriptor.commandTypes = indexed ? .drawIndexed : .draw
        descriptor.inheritPipelineState = true
        descriptor.inheritBuffers = true
        guard
            let icb = gpu.device.makeIndirectCommandBuffer(
                descriptor: descriptor, maxCommandCount: shape.draws.count, options: []
            )
        else { throw TestFailure("no indirect command buffer") }
        encoder.setVertexBuffer(try gpu.buffer(shape.draws[0].points), offset: 0, index: 0)
        for (index, draw) in shape.draws.enumerated() {
            let command = icb.indirectRenderCommandAt(index)
            if let indices = draw.indices {
                let buffer = try gpu.buffer(indices)
                encoder.useResource(buffer, usage: .read, stages: .vertex)
                command.drawIndexedPrimitives(
                    draw.primitive, indexCount: draw.count, indexType: .uint16, indexBuffer: buffer,
                    indexBufferOffset: 0, instanceCount: draw.instances, baseVertex: 0, baseInstance: 0
                )
            } else {
                command.drawPrimitives(
                    draw.primitive, vertexStart: draw.vertexStart, vertexCount: draw.count,
                    instanceCount: draw.instances, baseInstance: 0
                )
            }
        }
        encoder.executeCommandsInBuffer(icb, range: 0 ..< shape.draws.count)
    }

    static func shown(_ c: SIMD3<UInt8>) -> String { "\(c.x)/\(c.y)/\(c.z)" }
}
