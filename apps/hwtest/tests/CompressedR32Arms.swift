import Foundation
import HWTestCore
import HWTestGPU
import HWTestShared
import Metal

/// gpu.gm20b.compressed_targets' R32 arms: R32 colour targets compress as C32_2CRA (private_comp.h), which no device
/// run has judged yet. A private 256x128 R32Float, R32Uint and R32Sint target, made where the facade binds it
/// compressed memory (`r32_<format>_not_compressed` otherwise, before and after its passes), takes seven passes in
/// turn, each read back word for word by the copy engine (blit) and, for R32Float and R32Uint, by a compute read
/// through the texture unit: a draw over every texel with nothing loaded, a clear to 0 (the boot table's zero entry: it
/// must count a ZBC hit), a clear to a value no entry holds, a clear to the word 0x3f400000, then each of the three
/// clears with the left half drawn over. 0x3f400000 collides with an entry another format put in the table: an R32
/// clear's words are (r, 0, 0, 0) (clear.h) and the ms4_f16 case added RGBA16Float (0.75, 0, 0, 0)
/// (CompressedTargetsTest.cases), whose texel, 0x00003a00, a GPU matching the entry across formats would leave in every
/// tile. `r32_<format>` lists each pass's `<blit words off>/<compute words off>`; FAIL on any word off, naming the pass
/// (`r32_<format>_<pass>`), or on an arm that could not run (`r32_<format>_error`).
enum CompressedR32Arms {
    static let width = 256, height = 128
    static let collisionWord: UInt32 = 0x3f40_0000

    struct Format {
        let name: String
        let format: MTLPixelFormat
        let fragment: String
        let drawn: UInt32
        let cleared: UInt32

        /// The clear colour's red whose word in this format is `word`
        func clearColour(_ word: UInt32) -> Double {
            switch format {
            case .r32Float: Double(Float(bitPattern: word))
            case .r32Sint: Double(Int32(bitPattern: word))
            default: Double(word)
            }
        }
    }

    struct Pass {
        let name: String
        let clear: UInt32?
        let drawnColumns: Int
    }

    static let formats = [
        Format(
            name: "float", format: .r32Float, fragment: "exact_fragment", drawn: Float(-2.5).bitPattern,
            cleared: Float(0.3).bitPattern
        ),
        Format(name: "uint", format: .r32Uint, fragment: "integer_uint_fragment", drawn: 0xdead_beef, cleared: 7),
        Format(
            name: "sint", format: .r32Sint, fragment: "integer_sint_fragment",
            drawn: UInt32(bitPattern: -123_456_789), cleared: UInt32(bitPattern: -7)
        ),
    ]

    static func passes(_ f: Format) -> [Pass] {
        [
            Pass(name: "draw", clear: nil, drawnColumns: width),
            Pass(name: "zero", clear: 0, drawnColumns: 0),
            Pass(name: "clear", clear: f.cleared, drawnColumns: 0),
            Pass(name: "collision", clear: collisionWord, drawnColumns: 0),
            Pass(name: "zero_draw", clear: 0, drawnColumns: width / 2),
            Pass(name: "clear_draw", clear: f.cleared, drawnColumns: width / 2),
            Pass(name: "collision_draw", clear: collisionWord, drawnColumns: width / 2),
        ]
    }

    static func run(_ gpu: GPU, _ metrics: inout Metrics, _ wrong: inout [String]) async {
        for f in formats {
            do {
                try await arm(gpu, f, &metrics, &wrong)
            } catch {
                metrics.set("r32_\(f.name)_error", String(describing: error).prefix(80).description)
                wrong.append("r32_\(f.name)_error")
            }
        }
    }

    static func arm(_ gpu: GPU, _ f: Format, _ metrics: inout Metrics, _ wrong: inout [String]) async throws {
        let target = try await CompressionSettled.remade(gpu.device) {
            try gpu.texture(f.format, width, height, usage: [.renderTarget, .shaderRead])
        }
        let made = CompressionSettled.comptags(gpu.device, target) ?? 0
        let state = try gpu.render(fragment: f.fragment, format: f.format)
        var offs: [String] = []
        for p in passes(f) {
            let before = GM20BFacade.compression(of: gpu.device)["zbc_hits"] ?? 0
            try await encode(gpu, f, p, target, state)
            let hits = (GM20BFacade.compression(of: gpu.device)["zbc_hits"] ?? 0) - before
            let want = (0 ..< width * height).map { t in
                t % width < p.drawnColumns ? f.drawn : p.clear ?? f.drawn
            }
            let blit = try await blitWords(gpu, target)
            let sampled = try await sampledWords(gpu, f, target)
            let blitOff = zip(blit, want).filter { $0 != $1 }.count
            let sampledOff = sampled.map { zip($0, want).filter { $0 != $1 }.count }
            offs.append("\(blitOff)/\(sampledOff.map { String($0) } ?? "-")")
            if blitOff > 0 || (sampledOff ?? 0) > 0 { wrong.append("r32_\(f.name)_\(p.name)") }
            if p.name == "zero" {
                metrics.set("r32_\(f.name)_zero_hits", hits)
                if hits == 0 { wrong.append("r32_\(f.name)_zero_no_hit") }
            }
        }
        let after = CompressionSettled.comptags(gpu.device, target) ?? 0
        metrics.set("r32_\(f.name)", offs.joined(separator: ","))
        metrics.set("r32_\(f.name)_comptags", "\(made):\(after)")
        if made != 1 || after != 1 { wrong.append("r32_\(f.name)_not_compressed") }
    }

    static func encode(
        _ gpu: GPU, _ f: Format, _ p: Pass, _ target: MTLTexture, _ state: MTLRenderPipelineState
    ) async throws {
        try await gpu.run { commands in
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            if let clear = p.clear {
                pass.colorAttachments[0].loadAction = .clear
                pass.colorAttachments[0].clearColor = MTLClearColor(
                    red: f.clearColour(clear), green: 0, blue: 0, alpha: 0
                )
            } else {
                pass.colorAttachments[0].loadAction = .dontCare
            }
            pass.colorAttachments[0].storeAction = .store
            let e = commands.makeRenderCommandEncoder(descriptor: pass)!
            if p.drawnColumns > 0 {
                e.setRenderPipelineState(state)
                var corners = GPU.pixelQuad(0, 0, p.drawnColumns, height, width: width, height: height)
                e.setVertexBytes(&corners, length: MemoryLayout<SIMD4<Float>>.stride * 4, index: 0)
                var value = SIMD4<UInt32>(f.drawn, 0, 0, 0)
                e.setFragmentBytes(&value, length: 16, index: 0)
                e.drawPrimitives(type: .triangleStrip, vertexStart: 0, vertexCount: 4)
            }
            e.endEncoding()
        }
    }

    static func blitWords(_ gpu: GPU, _ target: MTLTexture) async throws -> [UInt32] {
        let bytes = try await gpu.read(target, bytesPerPixel: 4)
        return stride(from: 0, to: bytes.count, by: 4).map { i in
            UInt32(bytes[i]) | UInt32(bytes[i + 1]) << 8 | UInt32(bytes[i + 2]) << 16 | UInt32(bytes[i + 3]) << 24
        }
    }

    /// The words a compute kernel reads through the texture unit; nil for R32Sint, which no HW Test kernel reads
    static func sampledWords(_ gpu: GPU, _ f: Format, _ target: MTLTexture) async throws -> [UInt32]? {
        switch f.format {
        case .r32Float:
            return try await gpu.texels(target).map(\.x.bitPattern)
        case .r32Uint:
            let count = width * height
            let out = try gpu.buffer(count * 4)
            let pipeline = try gpu.compute("read_uint_view")
            try await gpu.run { commands in
                let e = commands.makeComputeCommandEncoder()!
                e.setComputePipelineState(pipeline)
                e.setTexture(target, index: 0)
                e.setBuffer(out, offset: 0, index: 0)
                e.dispatchThreads(
                    MTLSize(width: width, height: height, depth: 1),
                    threadsPerThreadgroup: MTLSize(width: 8, height: 8, depth: 1)
                )
                e.endEncoding()
            }
            return Array(
                UnsafeBufferPointer(start: out.contents().bindMemory(to: UInt32.self, capacity: count), count: count)
            )
        default:
            return nil
        }
    }
}
