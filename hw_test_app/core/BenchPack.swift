import Foundation

/// A pack experiment (hw_test_app/bench/switch_bench.h switch_bench_pack): its state's own programs drawn over quads in
/// one command buffer, each with the caller's uniform blocks, in one form of a split draw.
public struct BenchPack: Equatable, Sendable {
    public let form: String
    /// The state's vertex function is Core Animation's fixed_vert_l?h_spc
    public let ca: Bool
    public let quads: [BenchQuad]
}

public struct BenchQuad: Equatable, Sendable {
    public let rect: [UInt32]
    public let blocks: [BenchBlock]
    /// The draw's own vertex buffer bytes (a split capture's), drawn as Metal primitive `primitive` from `first`,
    /// `count` vertices; nil: the template's quad over rect
    public var vertices: Data?
    public var primitive: UInt32 = 0
    public var first: UInt32 = 0
    public var count: UInt32 = 0
}

/// `data` at `offset` in the quad's own 64 KiB region of one buffer, bound at `slot` of `stage` (1 vertex, 2 fragment)
public struct BenchBlock: Equatable, Sendable {
    public let stage: UInt32
    public let slot: UInt32
    public let offset: UInt32
    public let data: Data
}

extension BenchFields {
    /// protocol.py _pack
    func pack(_ value: Any?, width: Int, height: Int) throws -> BenchPack {
        guard let p = value as? [String: Any] else { throw BenchInvalid("pack is an object") }
        try Self.keys(p, BenchProtocol.packKeys, "pack")
        guard let form = p["form"] as? String, BenchProtocol.packForms.contains(form) else {
            throw BenchInvalid("pack.form is one of \(BenchProtocol.packForms.joined(separator: ", "))")
        }
        var ca = false
        if let flag = p["ca"] {
            guard let on = flag as? Bool else { throw BenchInvalid("pack.ca is true or false") }
            ca = on
        }
        guard let list = p["quads"] as? [Any], (1...BenchProtocol.maxQuads).contains(list.count) else {
            throw BenchInvalid("pack.quads: 1..\(BenchProtocol.maxQuads)")
        }
        let quads = try list.map { item -> BenchQuad in
            guard let quad = item as? [String: Any] else { throw BenchInvalid("pack.quads: objects") }
            try Self.keys(quad, BenchProtocol.quadKeys, "pack.quads[]")
            let rect = try counts(quad["rect"] ?? ([] as [Any]), 4, 0...BenchProtocol.maxSide, "pack.quads[].rect")
            guard rect[0] < rect[2], rect[2] <= width, rect[1] < rect[3], rect[3] <= height else {
                throw BenchInvalid("pack.quads[].rect outside the target")
            }
            guard let ubos = (quad["ubos"] ?? ([] as [Any])) as? [Any], ubos.count <= BenchProtocol.maxUbos else {
                throw BenchInvalid("pack.quads[].ubos: at most \(BenchProtocol.maxUbos)")
            }
            var built = BenchQuad(rect: rect.map(UInt32.init), blocks: try ubos.map(block))
            if let text = quad["vertices"] {
                let vertices = try bytes(text, "pack.quads[].vertices")
                guard (1...BenchProtocol.maxVertexBytes).contains(vertices.count) else {
                    throw BenchInvalid("pack.quads[].vertices: 1..\(BenchProtocol.maxVertexBytes) bytes")
                }
                built.vertices = vertices
                built.primitive = UInt32(try count(quad["primitive"], 0...4, "pack.quads[].primitive"))
                built.first = UInt32(try count(quad["first"] ?? (0 as Any), 0...65535, "pack.quads[].first"))
                built.count = UInt32(try count(quad["count"], 1...65535, "pack.quads[].count"))
            } else if quad["primitive"] != nil || quad["first"] != nil || quad["count"] != nil {
                throw BenchInvalid("pack.quads[]: primitive, first and count only with vertices")
            }
            return built
        }
        return BenchPack(form: form, ca: ca, quads: quads)
    }

    private func block(_ item: Any) throws -> BenchBlock {
        guard let ubo = item as? [String: Any] else { throw BenchInvalid("pack.quads[].ubos: objects") }
        try Self.keys(ubo, BenchProtocol.uboKeys, "pack.quads[].ubos[]")
        let stage = try count(ubo["stage"], 1...2, "pack.quads[].ubos[].stage")
        let slot = try count(ubo["slot"], 0...15, "pack.quads[].ubos[].slot")
        let offset = try count(
            ubo["offset"] ?? (0 as Any), 0...(BenchProtocol.quadRegion - 1), "pack.quads[].ubos[].offset")
        let data = try bytes(ubo["data"] ?? ("" as Any), "pack.quads[].ubos[].data")
        guard !data.isEmpty, offset + data.count <= BenchProtocol.quadRegion else {
            throw BenchInvalid("pack.quads[].ubos[]: data inside 64 KiB")
        }
        return BenchBlock(stage: UInt32(stage), slot: UInt32(slot), offset: UInt32(offset), data: data)
    }
}
