import Foundation
import HWTestCore
import HWTestGPU
import Metal
import MetalPerformanceShaders
import MetalPerformanceShadersGraph

/// The callers of the predication SPI on the raw path (drivers/TegraGM20BMetal/raw/runtime/raw_predication.h), with
/// supportsCommandBufferJump YES: MPSGraph's while, for and if, alone and nested as MPSGraph nests them (a while inside an
/// if, a for inside a while, an if inside a for, a for three loops deep), each count past its loop's copies at that depth
/// (1024 outermost, 128 inside one loop, 16 inside two), so the submission lays it again (loop_continuations), one
/// MPSCommandBuffer each; and MPSImageCanny's hysteresis on a square. PASS: every command buffer completes on the raw path
/// with MPSGraph's answers and Canny's edges, and a diagnostic build counted continuations; SKIP while the device answers
/// supportsCommandBufferJump NO.
struct MPSControlFlowTest: HWTest {
    let id = "gpu.raw.mps_control_flow"
    let name = "MPSGraph while/for/if and MPSImageCanny on the raw path's predicated loops"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 120 }

    static let whileCount: Int32 = 3000, forCount: Int32 = 37, side = 64
    static let whileInIfCount: Int32 = 1500, forInWhileCount: Int32 = 300, ifInForCount: Int32 = 1100
    static let threeDeepCount: Int32 = 20

    func run(_ context: TestContext) async throws -> Outcome {
        let gpu = try GPU.make()
        let kind = RawFacade.kind(of: gpu.device)
        guard kind == "raw" else { return .skip("the default device is not the raw facade (\(kind))") }
        guard MPSSupportsMTLDevice(gpu.device) else { return .fail("MPSSupportsMTLDevice is false") }
        let jump = (gpu.device as AnyObject).value(forKey: "supportsCommandBufferJump") as? Bool ?? false
        guard jump else {
            return .skip("supportsCommandBufferJump answers NO: MPSGraph and MPSImageCanny take their CPU paths")
        }
        var metrics = Metrics()
        var wrong: [String] = []
        let before = RawFacade.counters(of: gpu.device)

        let (counted, whilePath) = try runGraph(gpu, start: 0) { graph, start in
            let one = graph.constant(1, shape: [1], dataType: .int32)
            let limit = graph.constant(Double(Self.whileCount), shape: [1], dataType: .int32)
            return graph.while(
                initialInputs: [start],
                before: { inputs, results in
                    results.add(inputs[0])
                    return graph.lessThan(inputs[0], limit, name: nil)
                },
                after: { args in [graph.addition(args[0], one, name: nil)] }, name: nil)[0]
        }
        metrics.set("while_result", Int(counted))
        metrics.set("while_path", whilePath)
        if counted != Self.whileCount { wrong.append("while") }

        let (summed, forPath) = try runGraph(gpu, start: Self.forCount) { graph, start in
            graph.for(
                numberOfIterations: start,
                initialBodyArguments: [graph.constant(0, shape: [1], dataType: .int32)],
                body: { _, args in
                    [graph.addition(args[0], graph.constant(2, shape: [1], dataType: .int32), name: nil)]
                }, name: nil)[0]
        }
        metrics.set("for_result", Int(summed))
        metrics.set("for_path", forPath)
        if summed != 2 * Self.forCount { wrong.append("for") }

        let (chosen, ifPath) = try runGraph(gpu, start: 3) { graph, start in
            let pick = graph.lessThan(start, graph.constant(5, shape: [1], dataType: .int32), name: nil)
            return graph.if(
                pick, then: { [graph.constant(11, shape: [1], dataType: .int32)] },
                else: { [graph.constant(22, shape: [1], dataType: .int32)] }, name: nil)[0]
        }
        metrics.set("if_result", Int(chosen))
        metrics.set("if_path", ifPath)
        if chosen != 11 { wrong.append("if") }

        var paths = [whilePath, forPath, ifPath]
        for shape in try nested(gpu) {
            metrics.set("\(shape.name)_result", Int(shape.result))
            metrics.set("\(shape.name)_path", shape.path)
            paths.append(shape.path)
            if shape.result != shape.expected { wrong.append(shape.name) }
        }

        let canny = try await cannyEdges(gpu)
        metrics.set("canny_edge_pixels", canny.edges)
        metrics.set("canny_inside_pixels", canny.inside)
        metrics.set("canny_path", canny.path)
        if canny.edges == 0 || canny.inside != 0 { wrong.append("canny") }

        let after = RawFacade.counters(of: gpu.device)
        if let counted = after["loop_continuations"] {
            let continued = counted - (before["loop_continuations"] ?? 0)
            metrics.set("loop_continuations", continued)
            if continued == 0 { wrong.append("no loop continued") }
        }
        for path in paths + [canny.path] where path != "raw" { wrong.append("path:\(path)") }
        return wrong.isEmpty ? .pass(metrics) : .fail(wrong.joined(separator: ","), metrics)
    }

    struct Nested {
        let name: String
        let result: Int32, expected: Int32
        let path: String
    }

    /// MPSGraph's nestings, each count fed so MPSGraph cannot fold it
    func nested(_ gpu: GPU) throws -> [Nested] {
        func int(_ graph: MPSGraph, _ value: Int32) -> MPSGraphTensor {
            graph.constant(Double(value), shape: [1], dataType: .int32)
        }
        /// `times` iterations of a while carrying `carried` through `body`
        func repeated(
            _ graph: MPSGraph, _ times: MPSGraphTensor, _ carried: MPSGraphTensor,
            _ body: @escaping (MPSGraphTensor) -> MPSGraphTensor
        ) -> MPSGraphTensor {
            graph.while(
                initialInputs: [int(graph, 0), carried],
                before: { inputs, results in
                    results.add(inputs[0])
                    results.add(inputs[1])
                    return graph.lessThan(inputs[0], times, name: nil)
                },
                after: { args in [graph.addition(args[0], int(graph, 1), name: nil), body(args[1])] }, name: nil)[1]
        }
        func counted(_ graph: MPSGraph, _ times: MPSGraphTensor, _ carried: MPSGraphTensor) -> MPSGraphTensor {
            graph.for(
                numberOfIterations: times, initialBodyArguments: [carried],
                body: { _, args in [graph.addition(args[0], int(graph, 1), name: nil)] }, name: nil)[0]
        }
        var shapes: [Nested] = []
        func shape(
            _ name: String, _ start: Int32, _ expected: Int32, _ build: (MPSGraph, MPSGraphTensor) -> MPSGraphTensor
        )
            throws
        {
            let (result, path) = try runGraph(gpu, start: start, build)
            shapes.append(Nested(name: name, result: result, expected: expected, path: path))
        }
        try shape("while_in_if", Self.whileInIfCount, Self.whileInIfCount) { graph, start in
            let pick = graph.lessThan(start, int(graph, 1 << 20), name: nil)
            return graph.if(
                pick,
                then: { [repeated(graph, start, int(graph, 0)) { graph.addition($0, int(graph, 1), name: nil) }] },
                else: { [int(graph, -1)] }, name: nil)[0]
        }
        try shape("for_in_while", Self.forInWhileCount, 3 * Self.forInWhileCount) { graph, start in
            repeated(graph, int(graph, 3), int(graph, 0)) { counted(graph, start, $0) }
        }
        let odd = Self.ifInForCount / 2
        try shape("if_in_for", Self.ifInForCount, (Self.ifInForCount - odd) + 3 * odd) { graph, start in
            graph.for(
                numberOfIterations: start, initialBodyArguments: [int(graph, 0)],
                body: { index, args in
                    let even = graph.equal(graph.modulo(index, int(graph, 2), name: nil), int(graph, 0), name: nil)
                    return graph.if(
                        even, then: { [graph.addition(args[0], int(graph, 1), name: nil)] },
                        else: { [graph.addition(args[0], int(graph, 3), name: nil)] }, name: nil)
                }, name: nil)[0]
        }
        try shape("three_deep", Self.threeDeepCount, 4 * Self.threeDeepCount) { graph, start in
            repeated(graph, int(graph, 2), int(graph, 0)) { outer in
                repeated(graph, int(graph, 2), outer) { counted(graph, start, $0) }
            }
        }
        return shapes
    }

    /// The graph's one int32 output, encoded into one MPSCommandBuffer, and that command buffer's path. `start` comes
    /// in as a fed placeholder so MPSGraph cannot fold the control flow into a constant on the CPU.
    func runGraph(
        _ gpu: GPU, start: Int32, _ build: (MPSGraph, MPSGraphTensor) -> MPSGraphTensor
    ) throws -> (
        Int32, String
    ) {
        let graph = MPSGraph()
        let placeholder = graph.placeholder(shape: [1], dataType: .int32, name: nil)
        let out = build(graph, placeholder)
        let fed = MPSGraphTensorData(try gpu.buffer([start]), shape: [1], dataType: .int32)
        let commands = MPSCommandBuffer(from: gpu.queue)
        let results = graph.encode(
            to: commands, feeds: [placeholder: fed], targetTensors: [out], targetOperations: nil,
            executionDescriptor: nil)
        commands.commit()
        commands.waitUntilCompleted()
        let root = commands.rootCommandBuffer
        guard root.status == .completed, let data = results[out] else {
            return (-1, "error:" + (root.error.map { "\($0)" } ?? "no result"))
        }
        var value: Int32 = 0
        data.mpsndarray().readBytes(&value, strideBytes: nil)
        return (value, RawFacade.path(of: root))
    }

    /// A white square on black through MPSImageCanny: edge pixels on the square's border, none well inside it
    func cannyEdges(_ gpu: GPU) async throws -> (edges: Int, inside: Int, path: String) {
        let side = Self.side
        var bytes = [UInt8](repeating: 0, count: side * side * 4)
        for y in 16..<48 {
            for x in 16..<48 {
                let at = (y * side + x) * 4
                bytes.replaceSubrange(at..<at + 4, with: [255, 255, 255, 255])
            }
        }
        let source = try gpu.texture(.rgba8Unorm, side, side, usage: [.shaderRead])
        let destination = try gpu.texture(.r8Unorm, side, side, usage: [.shaderRead, .shaderWrite])
        try await gpu.upload(bytes, to: source, bytesPerRow: side * 4)
        let canny = MPSImageCanny(device: gpu.device)
        let timing = try await gpu.run(tolerateError: true) { commands in
            canny.encode(commandBuffer: commands, sourceTexture: source, destinationTexture: destination)
        }
        let out = try await gpu.read(destination, bytesPerPixel: 1)
        var edges = 0, inside = 0
        for y in 0..<side {
            for x in 0..<side where out[y * side + x] > 0 {
                if (20..<44).contains(x) && (20..<44).contains(y) { inside += 1 } else { edges += 1 }
            }
        }
        return (edges, inside, timing.path)
    }
}
