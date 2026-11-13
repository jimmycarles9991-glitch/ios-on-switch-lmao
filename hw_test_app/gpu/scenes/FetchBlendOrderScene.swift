import Foundation
import HWTestCore
import Metal

/// Draws that fetch the colour they blend into: the fragment reads [[color(n)]] and the pipeline blends One/One on that
/// attachment, so the blend's destination is the value the fetch read (fetch_blend_step). Each shape draws overlapping
/// layers into a 64x64 target in RGBA8, BGRA8 and RGBA16Float, and every pixel must equal the CPU's replay of its layers
/// in primitive order (FetchBlendState): a lost fragment shortens the count, a reordered one changes the tag fold. The
/// shapes: indexed strips with restarts, 16- and 32-bit (0xFFFF a vertex in the latter); a 300-triangle list; 8
/// instances; a fragment that folds [[primitive_id]] in; a memoryless second attachment blended across four draws and
/// copied into the first by the pass's last draw; indirect draws, plain and indexed strips; and RenderBox's quads, one
/// indexed strip of four vertices a draw. The indexed strips and the quads draw into BGR10A2Unorm as well, the format a
/// RenderBox state fetches whose storage form needs a packed view.
///
/// <shape>=ok|bad each; a bad shape adds <shape>_wrong (pixels, its three formats together) and <shape>_first
/// (format:x,y:got:r/g/b/a:want:r/g/b/a in steps). On the raw facade fetch_<shape> is how the fetch counters say it
/// kept order, warm_<shape> the runs drawn by m2v until the state was made, and a pass that is still not drawn raw
/// fails. The frozen m2v device is not run.
struct FetchBlendOrderScene: GPUScene {
    let id = "gpu.raw.fetch_blend_order"
    let name = "Self-blending framebuffer fetch: overlapping draws of every shape, in primitive order"

    static let size = FetchBlendMesh.size
    static let formats: [TexelFormat] = [.rgba8Unorm, .bgra8Unorm, .rgba16Float]
    static let warmups = 20
    static let wideBase = 65_530

    enum Shape: String, CaseIterable {
        case strip16, strip32, list300, instanced, quad4
        case primitiveID = "primitive_id"
        case memoryless, indirect
        case indirectStrip = "indirect_strip"
    }

    static let strips: [FetchBlendStrip] = [
        .folded(triangles: 12, bits: 0), .band(xs: [4, 44], y0: 8, y1: 56, tag: 2), .stub(vertices: 2),
        .folded(triangles: 9, bits: 2), .band(xs: [10, 20, 40, 60], y0: 30, y1: 62, tag: 1), .stub(vertices: 0),
        .band(xs: [24, 64], y0: 0, y1: 40, tag: 3), .folded(triangles: 7, bits: 0),
        .band(xs: [0, 30], y0: 20, y1: 64, tag: 0), .folded(triangles: 4, bits: 2),
    ]

    struct Plan {
        var mesh: FetchBlendMesh
        var shifts: [FetchBlendShift] = [.none]
        /// The memoryless pass's draws, as vertex ranges.
        var draws: [Range<Int>] = []

        var layers: [FetchBlendLayer] { shifts.flatMap { shift in mesh.layers.map { $0.moved(shift) } } }
    }

    static func plan(_ shape: Shape) -> Plan {
        switch shape {
        case .strip16, .indirectStrip:
            return Plan(mesh: .strips(strips))
        case .strip32:
            return Plan(mesh: .strips(strips, base: wideBase))
        case .list300:
            return Plan(mesh: .list(FetchBlendMesh.pieces(triangles: 300, seed: 0, rectEvery: 25)))
        case .instanced:
            let pieces: [FetchBlendPiece] = [
                .rect(4, 4, 36, 28, 1), .whole(2), .rect(20, 16, 52, 60, 3), .rect(0, 40, 30, 64, 0),
            ]
            let shifts = (0..<8).map {
                FetchBlendShift(dx: ($0 * 5) % 13 - 4, dy: ($0 * 3) % 11 - 3, tag: UInt32($0 & 3))
            }
            return Plan(mesh: .list(pieces), shifts: shifts)
        case .primitiveID:
            return Plan(mesh: .primitivePairs(60))
        case .memoryless:
            var mesh = FetchBlendMesh()
            var draws: [Range<Int>] = []
            for draw in 0..<4 {
                let first = mesh.points.count
                for piece in FetchBlendMesh.pieces(triangles: 24, seed: 40 * draw + 3) { mesh.add(piece) }
                draws.append(first..<mesh.points.count)
            }
            return Plan(mesh: mesh, draws: draws)
        case .indirect:
            return Plan(mesh: .list(FetchBlendMesh.pieces(triangles: 120, seed: 11)))
        case .quad4:
            return Plan(mesh: .quads(24))
        }
    }

    static func formats(_ shape: Shape) -> [TexelFormat] {
        switch shape {
        case .strip16, .quad4: return formats + [.bgr10a2Unorm]
        default: return formats
        }
    }

    static func short(_ format: TexelFormat) -> String {
        switch format {
        case .rgba8Unorm: return "rgba8"
        case .bgra8Unorm: return "bgra8"
        case .bgr10a2Unorm: return "bgr10a2"
        default: return "rgba16f"
        }
    }

    struct Tally {
        var wrong = 0
        var first: String?
        var warm = 0
        var problems: [String] = []
    }

    func run(_ gpu: GPU) async throws -> SceneResult {
        var result = SceneResult()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind != "m2v" else {
            result.notes.append("drawn on the raw facade and on Apple GPUs only")
            return result
        }
        for shape in Shape.allCases {
            let plan = Self.plan(shape)
            let before = RawFacade.counters(of: gpu.device)
            var tally = Tally()
            for format in Self.formats(shape) {
                let want = FetchBlendState.replay(plan.layers, size: Self.size, format: format)
                result.expect(want.allSatisfy { $0.max() <= 255 }, "\(shape.rawValue): the replay saturates")
                do {
                    try await check(shape, plan, want, format, gpu, &result, &tally)
                } catch {
                    tally.problems.append("\(Self.short(format)) \(GPU.describe(error))")
                }
            }
            let routes = RawFacade.fetchRoutes(before: before, after: RawFacade.counters(of: gpu.device))
            report(shape, tally, routes: kind == "raw" ? routes : nil, &result)
        }
        return result
    }

    private func check(
        _ shape: Shape, _ plan: Plan, _ want: [SIMD4<Int>], _ format: TexelFormat, _ gpu: GPU,
        _ result: inout SceneResult, _ tally: inout Tally
    ) async throws {
        let resources = try Resources(shape, plan, format, gpu)
        let draw = { (commands: MTLCommandBuffer) in Self.encode(shape, plan, resources, commands) }
        var timing = try await gpu.run(tolerateError: true, draw)
        var warm = 0
        while RawFacade.kind(of: gpu.device) == "raw", warm < Self.warmups, timing.path.hasPrefix("m2v:state") {
            warm += 1
            try await Task.sleep(nanoseconds: 50_000_000)
            timing = try await gpu.run(tolerateError: true, draw)
        }
        tally.warm += warm
        guard !timing.path.hasPrefix("error:") else {
            tally.problems.append("\(Self.short(format)) \(timing.path)")
            return
        }
        if RawFacade.kind(of: gpu.device) == "raw" && timing.path != "raw" {
            tally.problems.append("\(Self.short(format)) drawn on \(timing.path)")
        }
        let got = try await gpu.read(resources.target, bytesPerPixel: format.bytesPerTexel)
        let diff = result.compare(
            "\(shape.rawValue) \(Self.short(format))", bytes: got, expected: FetchBlendState.bytes(want, format),
            format: format, width: Self.size, height: Self.size, tolerance: 0)
        guard !diff.matches else { return }
        tally.wrong += diff.wrong
        if tally.first == nil {
            let (x, y) = (diff.first[0], diff.first[1])
            let seen = FetchBlendState.units(got, format, pixel: y * Self.size + x)
            tally.first =
                "\(Self.short(format)):\(x),\(y):got:\(FetchBlendState.shown(seen))"
                + ":want:\(FetchBlendState.shown(want[y * Self.size + x]))"
        }
    }

    private func report(_ shape: Shape, _ tally: Tally, routes: String?, _ result: inout SceneResult) {
        let name = shape.rawValue
        result.metrics.set(name, tally.wrong == 0 && tally.problems.isEmpty ? "ok" : "bad")
        if tally.wrong > 0 {
            result.metrics.set(name + "_wrong", tally.wrong)
            result.metrics.set(name + "_first", tally.first ?? "-")
            result.expect(false, name)
        }
        if let routes { result.metrics.set("fetch_" + name, routes) }
        if tally.warm > 0 { result.metrics.set("warm_" + name, tally.warm) }
        for problem in tally.problems { result.expect(false, "\(name): \(problem)") }
    }
}

extension FetchBlendOrderScene {
    struct Resources {
        let target: MTLTexture
        let second: MTLTexture?
        let pipeline: MTLRenderPipelineState
        let resolve: MTLRenderPipelineState?
        let points: MTLBuffer
        let instances: MTLBuffer
        let whole: MTLBuffer
        let indices: MTLBuffer?
        let arguments: MTLBuffer?
        let scale: Float

        init(_ shape: Shape, _ plan: Plan, _ format: TexelFormat, _ gpu: GPU) throws {
            let size = FetchBlendOrderScene.size
            let mesh = plan.mesh
            let memoryless = shape == .memoryless
            let fragment =
                shape == .primitiveID
                ? "fetch_blend_primitive_fragment" : memoryless ? "fetch_blend_second_fragment" : "fetch_blend_fragment"
            target = try gpu.texture(format.metal, size, size)
            second =
                try memoryless
                ? gpu.texture(format.metal, size, size, usage: [.renderTarget], storage: .memoryless) : nil
            pipeline = try FetchBlendPipeline.make(
                gpu, fragment: fragment, format: format.metal, attachments: memoryless ? 2 : 1,
                written: memoryless ? 1 : 0)
            resolve =
                try memoryless
                ? FetchBlendPipeline.make(
                    gpu, fragment: "fetch_blend_resolve_fragment", format: format.metal, blend: false, attachments: 2)
                : nil
            points = try gpu.buffer(mesh.points)
            instances = try gpu.buffer(plan.shifts.map { $0.clip(size: size) })
            whole = try gpu.buffer(FetchBlendMesh.whole.map { SIMD4<Float>($0.x, $0.y, 0, 0) })
            switch shape {
            case .strip16, .indirectStrip, .quad4: indices = try gpu.buffer(mesh.indices16)
            case .strip32: indices = try gpu.buffer(mesh.indices)
            default: indices = nil
            }
            switch shape {
            case .indirect: arguments = try gpu.buffer([UInt32(mesh.points.count), 1, 0, 0])
            case .indirectStrip: arguments = try gpu.buffer([UInt32(mesh.indices.count), 1, 0, 0, 0])
            default: arguments = nil
            }
            scale = FetchBlendState.scale(format)
        }
    }

    static func encode(_ shape: Shape, _ plan: Plan, _ resources: Resources, _ commands: MTLCommandBuffer) {
        let clear = FetchBlendState.clear
        let scale = Double(resources.scale)
        let cleared = MTLClearColor(
            red: Double(clear.x) / scale, green: Double(clear.y) / scale, blue: Double(clear.z) / scale,
            alpha: Double(clear.w) / scale)
        let pass = MTLRenderPassDescriptor()
        pass.colorAttachments[0].texture = resources.target
        pass.colorAttachments[0].loadAction = .clear
        pass.colorAttachments[0].clearColor = resources.second == nil ? cleared : MTLClearColor()
        pass.colorAttachments[0].storeAction = .store
        if let second = resources.second {
            pass.colorAttachments[1].texture = second
            pass.colorAttachments[1].loadAction = .clear
            pass.colorAttachments[1].clearColor = cleared
            pass.colorAttachments[1].storeAction = .dontCare
        }
        let encoder = commands.makeRenderCommandEncoder(descriptor: pass)!
        encoder.setRenderPipelineState(resources.pipeline)
        FetchBlendPipeline.bind(
            encoder, points: resources.points, instances: resources.instances, scale: resources.scale)
        let vertices = plan.mesh.points.count
        switch shape {
        case .strip16, .strip32:
            encoder.drawIndexedPrimitives(
                type: .triangleStrip, indexCount: plan.mesh.indices.count,
                indexType: shape == .strip16 ? .uint16 : .uint32, indexBuffer: resources.indices!, indexBufferOffset: 0)
        case .list300, .primitiveID:
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices)
        case .instanced:
            encoder.drawPrimitives(
                type: .triangle, vertexStart: 0, vertexCount: vertices, instanceCount: plan.shifts.count)
        case .memoryless:
            for draw in plan.draws {
                encoder.drawPrimitives(type: .triangle, vertexStart: draw.lowerBound, vertexCount: draw.count)
            }
            encoder.setRenderPipelineState(resources.resolve!)
            encoder.setVertexBuffer(resources.whole, offset: 0, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        case .indirect:
            encoder.drawPrimitives(type: .triangle, indirectBuffer: resources.arguments!, indirectBufferOffset: 0)
        case .indirectStrip:
            encoder.drawIndexedPrimitives(
                type: .triangleStrip, indexType: .uint16, indexBuffer: resources.indices!, indexBufferOffset: 0,
                indirectBuffer: resources.arguments!, indirectBufferOffset: 0)
        case .quad4:
            for quad in 0..<(plan.mesh.indices.count / 4) {
                encoder.drawIndexedPrimitives(
                    type: .triangleStrip, indexCount: 4, indexType: .uint16, indexBuffer: resources.indices!,
                    indexBufferOffset: quad * 8)
            }
        }
        encoder.endEncoding()
    }
}
