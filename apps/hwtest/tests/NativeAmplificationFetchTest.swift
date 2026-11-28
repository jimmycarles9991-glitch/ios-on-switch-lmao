import Foundation
import HWTestCommon
import HWTestCore
import HWTestGPU
import Metal

/// Framebuffer fetch under vertex amplification, as Weather's VFX particles draw: a state of
/// maxVertexAmplificationCount 2 beside a Depth32Float_Stencil8 attachment (depth written), blended One,
/// OneMinusSourceAlpha, both amplifications covering the target so that the second reads the first's result.
/// RGBA16Float, RGBA8Unorm and RG11B10Float targets.
/// - Screen arms: the fragment function screens 0.5 over the [[color(0)]] it reads at an alpha of 1, at counts 1 and
///   2, for a function no test drops a write of (the storage form under early tests) and for one that discards (the
///   storage form of the draw's in-shader tests). Every channel is exact in each float format. Each is drawn with the
///   fetch rung forced to the storage form (debug builds; a screen is a blend a fetch-free form could take) and on the
///   route the driver picks (`_natural`).
/// - ROP arms: an inexact source at an alpha of 0.5 drawn by a state that does not fetch (the ROP blends and stores)
///   and by one that reads [[color(0)]] and returns the same source (the storage form blends and stores in its
///   program, forced to the storage form): the two targets hold the same bytes where the ROP stores as the storage
///   write does.
struct NativeAmplificationFetchTest: NativeObjectTest {
    let id = "metal.native.amplification_fetch"
    let name = "An amplified state fetches the colour each amplification leaves, in order"

    static let size = 32
    static let clear: [Double] = [0, 0.5, 0.75, 1]

    static let source = """
    #include <metal_stdlib>
    using namespace metal;
    struct Out { float4 position [[position]]; };
    constant half4 inexact = half4(0.3h, 0.6h, 0.9h, 0.5h);
    vertex Out amplified_fetch_vertex(uint v [[vertex_id]]) {
        Out o;
        o.position = float4(float(v & 1) * 2 - 1, float(v >> 1) * 2 - 1, 0.5, 1);
        return o;
    }
    fragment half4 amplified_screen(Out in [[stage_in]], half4 c [[color(0)]]) {
        return half4(1 - (1 - half3(0.5h)) * (1 - c.rgb), 1);
    }
    fragment half4 amplified_screen_discard(Out in [[stage_in]], half4 c [[color(0)]]) {
        if (in.position.x < 0) discard_fragment();
        return half4(1 - (1 - half3(0.5h)) * (1 - c.rgb), 1);
    }
    fragment half4 amplified_rop(Out in [[stage_in]]) { return inexact; }
    fragment half4 amplified_rop_fetching(Out in [[stage_in]], half4 c [[color(0)]]) {
        return in.position.x < 0 ? c : inexact;
    }
    """

    struct Target {
        let name: String
        let format: MTLPixelFormat
        let bytes: Int
        /// The texel's red, green and blue
        let decode: (ArraySlice<UInt8>) -> [Double]
        /// A value as the store keeps it
        let store: (Double) -> Double
        /// How far a channel may land from the value computed in doubles: a code of RGBA8Unorm, which the function's
        /// `half` arithmetic reaches from a stored code
        let slack: Double
    }

    static func half(_ p: ArraySlice<UInt8>, _ k: Int) -> Double {
        let i = p.startIndex + 2 * k
        return Double(Float16(bitPattern: UInt16(p[i]) | UInt16(p[i + 1]) << 8))
    }

    /// An unsigned float of `mantissa` bits and a 5-bit exponent, biased 15
    static func ufloat(_ bits: UInt32, mantissa: UInt32) -> Double {
        let m = Double(bits & (1 << mantissa - 1)), e = Int(bits >> mantissa)
        let scale = Double(1 << mantissa)
        return e == 0 ? m / scale * pow(2, -14) : (1 + m / scale) * pow(2, Double(e - 15))
    }

    static func unorm8(_ p: ArraySlice<UInt8>) -> [Double] { (0 ..< 3).map { Double(p[p.startIndex + $0]) / 255 } }

    static func packed(_ p: ArraySlice<UInt8>) -> [Double] {
        let w = p.withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        return [ufloat(w & 0x7FF, mantissa: 6), ufloat(w >> 11 & 0x7FF, mantissa: 6), ufloat(w >> 22, mantissa: 5)]
    }

    static let targets = [
        Target(name: "rgba16f", format: .rgba16Float, bytes: 8, decode: { p in (0 ..< 3).map { half(p, $0) } },
               store: { $0 }, slack: 1e-6),
        Target(name: "rgba8", format: .rgba8Unorm, bytes: 4, decode: unorm8, store: { ($0 * 255).rounded() / 255 },
               slack: 1.01 / 255),
        Target(name: "rg11b10f", format: .rg11b10Float, bytes: 4, decode: packed, store: { $0 }, slack: 1e-6),
    ]

    /// The clear screened by 0.5 once per amplification, stored after each
    static func expected(_ t: Target, _ count: Int) -> [Double] {
        (0 ..< 3).map { k in
            var d = t.store(clear[k])
            for _ in 0 ..< count { d = t.store(1 - 0.5 * (1 - d)) }
            return d
        }
    }

    struct Scene {
        let gpu: GPU
        let library: MTLLibrary
        let depth: MTLTexture
        let tests: MTLDepthStencilState

        func pipeline(_ fragment: String, _ format: MTLPixelFormat) async throws -> MTLRenderPipelineState {
            let d = MTLRenderPipelineDescriptor()
            d.vertexFunction = library.makeFunction(name: "amplified_fetch_vertex")
            d.fragmentFunction = library.makeFunction(name: fragment)
            d.maxVertexAmplificationCount = 2
            d.depthAttachmentPixelFormat = .depth32Float_stencil8
            d.stencilAttachmentPixelFormat = .depth32Float_stencil8
            let c = d.colorAttachments[0]!
            c.pixelFormat = format
            c.isBlendingEnabled = true
            c.sourceRGBBlendFactor = .one
            c.destinationRGBBlendFactor = .oneMinusSourceAlpha
            c.sourceAlphaBlendFactor = .one
            c.destinationAlphaBlendFactor = .oneMinusSourceAlpha
            return try await gpu.device.makeRenderPipelineState(descriptor: d)
        }

        /// A target of `t` cleared to the clear colour and drawn once by `pipeline` at `count`, as read back; with
        /// `forced`, its fetch rung the storage form's where the build has the probe (`forced` false where it has not)
        func draw(
            _ pipeline: MTLRenderPipelineState, _ t: Target, _ count: Int, forced: inout Bool, _ work: inout NativeWork
        ) async throws -> [UInt8] {
            let target = try gpu.texture(t.format, NativeAmplificationFetchTest.size, NativeAmplificationFetchTest.size)
            let clear = NativeAmplificationFetchTest.clear
            let render = { try await gpu.run { commands in
                let pass = MTLRenderPassDescriptor()
                pass.colorAttachments[0].texture = target
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: clear[0], green: clear[1], blue: clear[2], alpha: clear[3]
                )
                pass.colorAttachments[0].storeAction = .store
                pass.depthAttachment.texture = depth
                pass.depthAttachment.loadAction = .clear
                pass.depthAttachment.clearDepth = 1
                pass.depthAttachment.storeAction = .dontCare
                pass.stencilAttachment.texture = depth
                pass.stencilAttachment.loadAction = .clear
                pass.stencilAttachment.storeAction = .dontCare
                let e = commands.makeRenderCommandEncoder(descriptor: pass)!
                e.setRenderPipelineState(pipeline)
                e.setDepthStencilState(tests)
                e.setVertexAmplificationCount(count, viewMappings: nil)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
                e.endEncoding()
            } }
            var timing = forced ? try await GM20BFacade.forcingFetchRung(4, on: gpu.device, render) : nil
            if timing == nil {
                forced = false
                timing = try await render()
            }
            work.paths.append(timing!.path)
            return try await gpu.read(target, bytesPerPixel: t.bytes)
        }
    }

    func exercise(_ gpu: GPU, _ metrics: inout Metrics) async throws -> NativeWork {
        let device = gpu.device
        var work = NativeWork()
        guard device.supportsVertexAmplificationCount(2) else {
            work.failures.append("supportsVertexAmplificationCount: answers NO for 2")
            return work
        }
        let library = try await device.makeLibrary(source: Self.source, options: nil)
        let tests = MTLDepthStencilDescriptor()
        tests.depthCompareFunction = .always
        tests.isDepthWriteEnabled = true
        guard let depthState = device.makeDepthStencilState(descriptor: tests) else {
            throw TestFailure("no depth stencil state")
        }
        let depth = try gpu.texture(.depth32Float_stencil8, Self.size, Self.size, usage: .renderTarget)
        work.objects.append(depth)
        let scene = Scene(gpu: gpu, library: library, depth: depth, tests: depthState)
        let texels = 0 ..< Self.size * Self.size
        var forcing = true
        for t in Self.targets {
            for fragment in ["amplified_screen", "amplified_screen_discard"] {
                let pipeline = try await scene.pipeline(fragment, t.format)
                work.objects.append(pipeline)
                for (count, forced) in [(1, true), (2, true), (1, false), (2, false)] {
                    var force = forced && forcing
                    let pixels = try await scene.draw(pipeline, t, count, forced: &force, &work)
                    forcing = forcing && (force || !forced)
                    let want = Self.expected(t, count)
                    let wrong = texels.filter { i in
                        let got = t.decode(pixels.pixel(i % Self.size, i / Self.size, width: Self.size,
                                                        bytesPerPixel: t.bytes))
                        return zip(got, want).contains { abs($0 - $1) > t.slack }
                    }.count
                    let function = fragment == "amplified_screen" ? "early" : "discard"
                    let arm = "\(function)_\(t.name)_count\(count)\(forced ? "" : "_natural")"
                    metrics.set("\(arm)_wrong_pixels", wrong)
                    if wrong != 0 { work.failures.append("\(arm): \(wrong) pixels wrong, want \(want)") }
                }
            }
            let rop = try await scene.pipeline("amplified_rop", t.format)
            let fetching = try await scene.pipeline("amplified_rop_fetching", t.format)
            work.objects += [rop, fetching]
            var unforced = false
            let byRop = try await scene.draw(rop, t, 2, forced: &unforced, &work)
            var force = forcing
            let byStorage = try await scene.draw(fetching, t, 2, forced: &force, &work)
            let differ = texels.filter { i in
                byRop.pixel(i % Self.size, i / Self.size, width: Self.size, bytesPerPixel: t.bytes) !=
                    byStorage.pixel(i % Self.size, i / Self.size, width: Self.size, bytesPerPixel: t.bytes)
            }.count
            metrics.set("rop_\(t.name)_differing_texels", differ)
            if differ != 0 { work.failures.append("rop_\(t.name): \(differ) texels differ from the ROP's") }
        }
        metrics.set("forced", forcing ? "storage form" : "no probe channel")
        return work
    }
}
