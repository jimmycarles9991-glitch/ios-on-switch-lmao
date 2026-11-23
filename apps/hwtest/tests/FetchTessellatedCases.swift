import Foundation
import HWTestCore
import HWTestGPU
import Metal

enum FetchTessellatedCases {
    static let side = 64
    static let interior = 2 ..< 62
    static let interiorPixels = 3600
    static let runs = 10

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct FetchTessParams { uint mode; float first, second, step; };
    struct FetchTessOut { float4 position [[position]]; float u; float v; uint group [[flat]]; };

    [[patch(quad, 4)]] vertex FetchTessOut fetch_tess_vertex(float2 uv [[position_in_patch]],
                                                             uint pid [[patch_id]], uint iid [[instance_id]],
                                                             constant FetchTessParams &p [[buffer(0)]]) {
        const float x = p.mode == 0 ? 1.0 - abs(2.0 * uv.x - 1.0) : uv.x;
        const float y = p.mode == 1 ? 1.0 - abs(2.0 * uv.y - 1.0) : uv.y;
        FetchTessOut o;
        o.position = float4(2.0 * x - 1.0, 2.0 * y - 1.0, 0.5, 1.0);
        o.u = uv.x;
        o.v = uv.y;
        o.group = p.mode == 2 ? pid : p.mode == 3 ? iid : 0;
        return o;
    }

    static float4 fetch_tess_mix(FetchTessOut in, float4 dst, constant FetchTessParams &p) {
        const bool first = p.mode == 0 ? in.u < 0.5 : p.mode == 1 ? in.v < 0.5 : in.group == 0;
        return float4(dst.r * 0.5 + (first ? p.first : p.second), dst.g + p.step, 0.0, 1.0);
    }

    fragment float4 fetch_tess_fragment(FetchTessOut in [[stage_in]], float4 dst [[color(0)]],
                                        constant FetchTessParams &p [[buffer(0)]]) {
        return fetch_tess_mix(in, dst, p);
    }

    struct FetchTessDepthOut { float4 colour [[color(0)]]; float depth [[depth(any)]]; };
    fragment FetchTessDepthOut fetch_tess_depth_fragment(FetchTessOut in [[stage_in]], float4 dst [[color(0)]],
                                                         constant FetchTessParams &p [[buffer(0)]]) {
        return {fetch_tess_mix(in, dst, p), 0.25};
    }

    struct FetchTessScreen { float4 position [[position]]; };
    vertex FetchTessScreen fetch_tess_screen(uint vid [[vertex_id]]) {
        const float2 corners[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
        return {float4(corners[vid], 0.5, 1)};
    }

    fragment float4 fetch_tess_sample_codes(FetchTessScreen in [[stage_in]], uint s [[sample_id]]) {
        return float4(float(s * 40u) / 255.0, 0.0, 0.0, 1.0);
    }

    fragment float4 fetch_tess_unpack(FetchTessScreen in [[stage_in]],
                                      texture2d_ms<float, access::read> samples [[texture(0)]]) {
        const uint2 at = uint2(in.position.xy);
        return samples.read(uint2(at.x / 4, at.y), at.x % 4);
    }
    """

    enum Mode: UInt32 { case foldU = 0, foldV, patches, instances }
    enum Submit { case direct, indirect, icb }
    enum Target { case bgra8, bgr5a1, ms4, depth }

    struct Params {
        var mode: UInt32
        var first, second, step: Float
    }

    struct Arm {
        let key: String
        var mode = Mode.foldU
        var patches = 1
        var instances = 1
        var factor = 2
        var submit = Submit.direct
        var target = Target.bgra8
        let apple: [(first: Int, second: Int)]

        var flat: Bool { mode == .patches || mode == .instances }
        var fragments: Int { mode == .patches ? patches : mode == .instances ? instances : 2 }
        var survivors: Int { target == .depth ? 1 : fragments }
        var samples: Int { target == .ms4 ? 4 : 1 }
        var format: MTLPixelFormat { target == .bgr5a1 ? .bgr5A1Unorm : .bgra8Unorm }
        var codes: (first: Int, second: Int, step: Int) { target == .bgr5a1 ? (12, 4, 4) : (96, 32, 8) }
        var maximum: Float { target == .bgr5a1 ? 31 : 255 }
        var params: Params {
            let c = codes
            return Params(
                mode: mode.rawValue, first: Float(c.first) / maximum, second: Float(c.second) / maximum,
                step: Float(c.step) / maximum
            )
        }
        var stateKey: String { "\(target)_\(submit == .icb)" }
    }

    static let arms: [Arm] = [
        Arm(key: "tess_u2", apple: [(2759, 841)]),
        Arm(key: "tess_v2", mode: .foldV, apple: [(2759, 841)]),
        Arm(key: "tess_u4", factor: 4, apple: [(2984, 616)]),
        Arm(key: "tess_v4", mode: .foldV, factor: 4, apple: [(2504, 1096)]),
        Arm(key: "tess_u8", factor: 8, apple: [(3288, 312)]),
        Arm(key: "tess_patches", mode: .patches, patches: 2, apple: [(3600, 0)]),
        Arm(key: "tess_instances", mode: .instances, instances: 2, apple: [(3600, 0)]),
        Arm(key: "tess_six", mode: .patches, patches: 6, apple: [(3600, 0)]),
        Arm(key: "tess_indirect_u2", submit: .indirect, apple: [(2759, 841)]),
        Arm(key: "tess_icb_u2", submit: .icb, apple: [(2759, 841)]),
        Arm(key: "tess_bgr5a1_u2", target: .bgr5a1, apple: [(2759, 841)]),
        Arm(key: "tess_ms4_u2", target: .ms4, apple: [(2730, 870), (2730, 870), (2788, 812), (2788, 812)]),
        Arm(key: "tess_depth_u2", target: .depth, apple: [(2759, 841)]),
    ]

    struct Observation {
        var error: NSError?
        var path = "-"
        var images: [[UInt16]] = []
    }

    struct Tally {
        var firstThenSecond = 0, secondThenFirst = 0, torn = 0, other = 0

        static func + (a: Tally, b: Tally) -> Tally {
            Tally(
                firstThenSecond: a.firstThenSecond + b.firstThenSecond,
                secondThenFirst: a.secondThenFirst + b.secondThenFirst, torn: a.torn + b.torn, other: a.other + b.other
            )
        }
    }

    static func blend(_ start: Int, _ steps: [Int]) -> Int { steps.reduce(start) { $0 / 2 + $1 } }

    static func tally(_ arm: Arm, _ image: [UInt16]) -> [Tally] {
        let c = arm.codes
        let seconds = [Int](repeating: c.second, count: arm.fragments - 1)
        let firstOrder = Array(([c.first] + seconds).prefix(arm.survivors))
        let secondOrder = Array((seconds + [c.first]).prefix(arm.survivors))
        let full = arm.survivors * c.step
        var tallies = [Tally](repeating: Tally(), count: arm.samples)
        for (index, code) in image.enumerated() {
            let sample = index % arm.samples
            let start = arm.target == .ms4 ? 40 * sample : 0
            let (r, g) = (Int(code >> 8), Int(code & 0xff))
            if g == full && r == blend(start, firstOrder) {
                tallies[sample].firstThenSecond += 1
            } else if g == full && r == blend(start, secondOrder) {
                tallies[sample].secondThenFirst += 1
            } else if g > 0 && g < full {
                tallies[sample].torn += 1
            } else {
                tallies[sample].other += 1
            }
        }
        return tallies
    }

    static func samplesDistinct(_ image: [UInt16]) -> Int {
        stride(from: 0, to: image.count, by: 4).filter { Set(image[$0 ..< $0 + 4].map { $0 >> 8 }).count == 4 }.count
    }

    static func interiorCodes(samples: Int, _ code: (Int, Int, Int) -> UInt16) -> [UInt16] {
        var codes: [UInt16] = []
        for y in interior {
            for x in interior {
                for s in 0 ..< samples { codes.append(code(x, y, s)) }
            }
        }
        return codes
    }

    final class Scene {
        let gpu: GPU
        let library: MTLLibrary
        private var states: [String: MTLRenderPipelineState] = [:]

        init(_ gpu: GPU) async throws {
            self.gpu = gpu
            library = try await gpu.device.makeLibrary(source: FetchTessellatedCases.source, options: nil)
        }

        func state(_ arm: Arm) async throws -> MTLRenderPipelineState {
            if let made = states[arm.stateKey] { return made }
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "fetch_tess_vertex")
            d.fragmentFunction = library.makeFunction(
                name: arm.target == .depth ? "fetch_tess_depth_fragment" : "fetch_tess_fragment"
            )
            d.colorAttachments[0].pixelFormat = arm.format
            d.rasterSampleCount = arm.samples
            d.depthAttachmentPixelFormat = arm.target == .depth ? .depth32Float : .invalid
            d.tessellationPartitionMode = .integer
            d.tessellationFactorFormat = .half
            d.tessellationFactorStepFunction = .constant
            d.tessellationOutputWindingOrder = .counterClockwise
            d.tessellationControlPointIndexType = .none
            d.maxTessellationFactor = 16
            d.supportIndirectCommandBuffers = arm.submit == .icb
            let made = try await gpu.device.makeRenderPipelineState(descriptor: d)
            states[arm.stateKey] = made
            return made
        }

        func screen(_ fragment: String, samples: Int) async throws -> MTLRenderPipelineState {
            if let made = states[fragment] { return made }
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "fetch_tess_screen")
            d.fragmentFunction = library.makeFunction(name: fragment)
            d.colorAttachments[0].pixelFormat = .bgra8Unorm
            d.rasterSampleCount = samples
            let made = try await gpu.device.makeRenderPipelineState(descriptor: d)
            states[fragment] = made
            return made
        }

        func less() throws -> MTLDepthStencilState {
            let d = MTLDepthStencilDescriptor()
            d.depthCompareFunction = .less
            d.isDepthWriteEnabled = true
            guard let less = gpu.device.makeDepthStencilState(descriptor: d) else {
                throw TestFailure("no depth state")
            }
            return less
        }

        struct Draw {
            let arm: Arm
            let state: MTLRenderPipelineState
            let target: MTLTexture
            let factors, indirect: MTLBuffer
            var commands: MTLIndirectCommandBuffer?
            var depth: MTLTexture?
            var less: MTLDepthStencilState?
            var sampleCodes, unpack: MTLRenderPipelineState?
            var unpacked: MTLTexture?
        }

        func observe(_ arm: Arm) async throws -> Observation {
            let factors = try gpu.buffer([Float16](repeating: Float16(arm.factor), count: 6))
            let arguments = MTLDrawPatchIndirectArguments(
                patchCount: UInt32(arm.patches), instanceCount: UInt32(arm.instances), patchStart: 0, baseInstance: 0
            )
            var draw = Draw(
                arm: arm, state: try await state(arm),
                target: try gpu.texture(arm.format, side, side, samples: arm.samples), factors: factors,
                indirect: try gpu.buffer([arguments])
            )
            if arm.submit == .icb { draw.commands = try patchCommands(arm, factors) }
            if arm.target == .depth {
                draw.depth = try gpu.texture(.depth32Float, side, side, usage: [.renderTarget])
                draw.less = try less()
            }
            if arm.target == .ms4 {
                draw.sampleCodes = try await screen("fetch_tess_sample_codes", samples: 4)
                draw.unpack = try await screen("fetch_tess_unpack", samples: 1)
                draw.unpacked = try gpu.texture(.bgra8Unorm, side * 4, side)
            }
            var seen = Observation()
            for _ in 0 ..< runs {
                let buffer = try drawn(draw)
                seen.path = GM20BFacade.path(of: buffer)
                if buffer.status == .error {
                    seen.error = (buffer.error as NSError?) ?? NSError(domain: "no error", code: -1)
                    return seen
                }
                seen.images.append(try await codes(draw))
            }
            return seen
        }

        func drawn(_ draw: Draw) throws -> MTLCommandBuffer {
            let buffer = try gpu.commit { commandBuffer in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = draw.target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 1)
                pass.colorAttachments[0].storeAction = .store
                if let depth = draw.depth {
                    pass.depthAttachment.texture = depth
                    pass.depthAttachment.loadAction = .clear
                    pass.depthAttachment.clearDepth = 1
                    pass.depthAttachment.storeAction = .dontCare
                }
                let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass)!
                if let sampleCodes = draw.sampleCodes {
                    encoder.setRenderPipelineState(sampleCodes)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                }
                encoder.setRenderPipelineState(draw.state)
                if let less = draw.less { encoder.setDepthStencilState(less) }
                var params = draw.arm.params
                encoder.setVertexBytes(&params, length: MemoryLayout<Params>.stride, index: 0)
                encoder.setFragmentBytes(&params, length: MemoryLayout<Params>.stride, index: 0)
                encoder.setTessellationFactorBuffer(draw.factors, offset: 0, instanceStride: 0)
                switch draw.arm.submit {
                case .direct:
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchStart: 0, patchCount: draw.arm.patches,
                        patchIndexBuffer: nil, patchIndexBufferOffset: 0, instanceCount: draw.arm.instances,
                        baseInstance: 0
                    )
                case .indirect:
                    encoder.drawPatches(
                        numberOfPatchControlPoints: 4, patchIndexBuffer: nil, patchIndexBufferOffset: 0,
                        indirectBuffer: draw.indirect, indirectBufferOffset: 0
                    )
                case .icb:
                    encoder.useResource(draw.factors, usage: .read, stages: .vertex)
                    encoder.executeCommandsInBuffer(draw.commands!, range: 0 ..< 1)
                }
                encoder.endEncoding()
            }
            buffer.waitUntilCompleted()
            return buffer
        }

        func patchCommands(_ arm: Arm, _ factors: MTLBuffer) throws -> MTLIndirectCommandBuffer {
            let d = MTLIndirectCommandBufferDescriptor()
            d.commandTypes = [.drawPatches]
            d.inheritPipelineState = true
            d.inheritBuffers = true
            guard let commands = gpu.device.makeIndirectCommandBuffer(descriptor: d, maxCommandCount: 1, options: [])
            else { throw TestFailure("no indirect command buffer") }
            commands.indirectRenderCommandAt(0).drawPatches(
                4, patchStart: 0, patchCount: arm.patches, patchIndexBuffer: nil, patchIndexBufferOffset: 0,
                instanceCount: arm.instances, baseInstance: 0, tessellationFactorBuffer: factors,
                tessellationFactorBufferOffset: 0, tessellationFactorBufferInstanceStride: 0
            )
            return commands
        }

        func codes(_ draw: Draw) async throws -> [UInt16] {
            if let unpack = draw.unpack, let unpacked = draw.unpacked {
                try await gpu.run { commandBuffer in
                    let pass = MTLRenderPassDescriptor()
                    pass.colorAttachments[0].texture = unpacked
                    pass.colorAttachments[0].loadAction = .dontCare
                    pass.colorAttachments[0].storeAction = .store
                    let encoder = commandBuffer.makeRenderCommandEncoder(descriptor: pass)!
                    encoder.setRenderPipelineState(unpack)
                    encoder.setFragmentTexture(draw.target, index: 0)
                    encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
                    encoder.endEncoding()
                }
                let bytes = try await gpu.read(unpacked, bytesPerPixel: 4)
                return interiorCodes(samples: 4) { x, y, s in
                    let at = 4 * (y * side * 4 + x * 4 + s)
                    return UInt16(bytes[at + 2]) << 8 | UInt16(bytes[at + 1])
                }
            }
            if draw.arm.target == .bgr5a1 {
                let bytes = try await gpu.read(draw.target, bytesPerPixel: 2)
                return interiorCodes(samples: 1) { x, y, _ in
                    let at = 2 * (y * side + x)
                    let word = UInt16(bytes[at]) | UInt16(bytes[at + 1]) << 8
                    return (word >> 10 & 31) << 8 | (word >> 5 & 31)
                }
            }
            let bytes = try await gpu.read(draw.target, bytesPerPixel: 4)
            return interiorCodes(samples: 1) { x, y, _ in
                let at = 4 * (y * side + x)
                return UInt16(bytes[at + 2]) << 8 | UInt16(bytes[at + 1])
            }
        }
    }
}
