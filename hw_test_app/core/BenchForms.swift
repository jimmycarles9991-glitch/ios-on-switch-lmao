import CoreFoundation
import Foundation

/// An experiment's 2D operation, its source, and the compressed and compare forms, checked as tools/benchloop/protocol.py
/// _forms checks them.
extension BenchFields {
    struct Forms {
        var twod: BenchTwod?
        var source: BenchSource?
        var compare: String?
        var compressed = false
    }

    func forms(_ kind: BenchKind, format: String, size: (Int, Int), rect: [Int]) throws -> Forms {
        var forms = Forms()
        if kind == .twod {
            forms.source = try source()
            forms.twod = try twod(format: format, size: size, rect: rect, source: forms.source)
        } else if fields["source"] != nil {
            throw BenchInvalid("only twod experiments read a source")
        }
        if let value = fields["compressed"] {
            guard kind == .readback, let flag = value as? Bool, flag else {
                throw BenchInvalid("compressed: a readback target")
            }
            forms.compressed = true
        }
        if let value = fields["compare"] {
            guard let compare = value as? String, BenchProtocol.compares[kind]?.contains(compare) == true else {
                throw BenchInvalid("compare: one of \(BenchProtocol.compares[kind] ?? []) for \(kind.rawValue)")
            }
            try check(compare: compare, forms, format: format, size: size, rect: rect)
            forms.compare = compare
        } else if kind == .twod, fields["program"] != nil {
            throw BenchInvalid("a twod experiment draws a program only to compare with it")
        }
        return forms
    }

    private func check(compare: String, _ forms: Forms, format: String, size: (Int, Int), rect: [Int]) throws {
        switch compare {
        case "plain":
            guard forms.compressed else {
                throw BenchInvalid("compare plain is the compressed target against a plain one")
            }
        case "program":
            guard forms.twod != nil else {
                throw BenchInvalid("compare program: a 2D operation against the program drawn over it")
            }
        case "copy":
            guard let t = forms.twod, t.op == .scale, t.du == 1 << 32, t.dv == 1 << 32, t.sx == 0, t.sy == 0,
                !t.bilinear
            else { throw BenchInvalid("compare copy: a 1:1 point scale") }
            let source = forms.source.map { ($0.format, Int($0.width), Int($0.height)) } ?? BenchProtocol.picture
            guard source.0 == format else {
                throw BenchInvalid("compare copy: the copy engine copies bytes, one format")
            }
            guard rect[2] - rect[0] <= source.1, rect[3] - rect[1] <= source.2 else {
                throw BenchInvalid("compare copy: the rect inside the source")
            }
        case "sampled3d":
            guard let t = forms.twod, t.op == .scale, t.bilinear, rect == [0, 0, size.0, size.1] else {
                throw BenchInvalid("compare sampled3d: a whole-target bilinear scale")
            }
            // the centre origin, or the production form: the corner origin offset by half a step (probeResolveOp)
            let centred = t.corner ? t.sx == t.du / 2 && t.sy == t.dv / 2 : t.sx == 0 && t.sy == 0
            guard centred else {
                throw BenchInvalid(
                    "compare sampled3d: pixel centres mapped, the centre origin or the corner with half a step")
            }
            guard BenchProtocol.mipFormats.contains(BenchProtocol.formats[format]!.raw) else {
                throw BenchInvalid("compare sampled3d: the pack has no sampled-quad state for it")
            }
        default:
            guard forms.twod?.op == .resolve else {
                throw BenchInvalid("compare resolve3d: a resolve against the pack's resolve state")
            }
        }
    }

    private func source() throws -> BenchSource? {
        guard let value = fields["source"] else { return nil }
        guard let s = value as? [String: Any] else { throw BenchInvalid("source is an object") }
        try Self.keys(s, BenchProtocol.sourceKeys, "source")
        guard let format = s["format"] as? String, let info = BenchProtocol.formats[format] else {
            throw BenchInvalid("source format is not one the raw path renders")
        }
        guard BenchProtocol.twodFormats.contains(info.raw) else {
            throw BenchInvalid("source format: the 2D engine has no surface format for it")
        }
        let width = try count(s["width"], 1...BenchProtocol.maxSide, "source.width")
        let height = try count(s["height"], 1...BenchProtocol.maxSide, "source.height")
        let data = try bytes(s["data"] ?? ("" as Any), "source.data")
        guard data.count == width * height * info.bytes else { throw BenchInvalid("source.data is the whole source") }
        return BenchSource(
            format: format, formatRaw: info.raw, width: UInt32(width), height: UInt32(height), data: data)
    }

    private func byScale(_ op: BenchTwodOp, _ t: [String: Any]) throws -> Bool {
        guard let value = t["byScale"] else { return false }
        guard let flag = value as? Bool, op == .resolve else { throw BenchInvalid("twod.byScale: a resolve's") }
        return flag
    }

    private func twod(format: String, size: (Int, Int), rect: [Int], source: BenchSource?) throws -> BenchTwod {
        guard let t = fields["twod"] as? [String: Any], let op = BenchTwodOp(rawValue: t["op"] as? String ?? "") else {
            throw BenchInvalid("twod is an object whose op is fill, scale or resolve")
        }
        try Self.keys(t, BenchProtocol.twodKeys, "twod")
        let color = try counts(t["color"] ?? ([0, 0, 0, 0] as [Any]), 4, 0...Int(UInt32.max), "twod.color")
        let wide = { (key: String, otherwise: UInt64) throws -> UInt64 in
            guard let v = t[key] else { return otherwise }
            guard let n = v as? NSNumber, CFGetTypeID(n) != CFBooleanGetTypeID(), n.int64Value >= 0 else {
                throw BenchInvalid("twod.\(key) is a 32.32 number")
            }
            return n.uint64Value
        }
        let flag = { (key: String, otherwise: Bool) throws -> Bool in
            guard let v = t[key] else { return otherwise }
            guard let b = v as? Bool else { throw BenchInvalid("twod.\(key) is true or false") }
            return b
        }
        let fmt2d = UInt32(try count(t["fmt2d"] ?? (0 as Any), 0...255, "twod.fmt2d"))
        guard fmt2d != 0 || BenchProtocol.twodFormats.contains(BenchProtocol.formats[format]!.raw) else {
            throw BenchInvalid("target format: the 2D engine has no surface format for it (give twod.fmt2d)")
        }
        let reference = try flag("reference", false)
        guard !reference || op == .fill else { throw BenchInvalid("twod.reference is the reference fill: fill only") }
        guard op != .fill || source == nil else { throw BenchInvalid("a fill reads no source") }
        let colorFormat = UInt32(try count(t["colorFormat"] ?? (0 as Any), 0...255, "twod.colorFormat"))
        let destination = fmt2d != 0 ? fmt2d : BenchProtocol.twodDestination[BenchProtocol.formats[format]!.raw] ?? 0
        guard op != .fill || (colorFormat == destination && BenchProtocol.solidFillFormats.contains(destination)) else {
            throw BenchInvalid("a fill's colorFormat is its destination's own 2D format, and not an sRGB one")
        }
        var samples = 0
        if op == .resolve {
            samples = try count(t["samples"], 2...4, "twod.samples")
            guard let grid = BenchProtocol.sampleGrids[samples] else { throw BenchInvalid("twod.samples is 2 or 4") }
            guard rect == [0, 0, size.0, size.1] else { throw BenchInvalid("a resolve writes the whole target") }
            let from =
                source.map { (Int($0.width), Int($0.height)) } ?? (BenchProtocol.picture.1, BenchProtocol.picture.2)
            guard size.0 << grid.0 <= from.0, size.1 << grid.1 <= from.1 else {
                throw BenchInvalid(
                    "a resolve's sample grid is copied from the source's corner: the source must cover it")
            }
        } else if t["samples"] != nil {
            throw BenchInvalid("twod.samples is a resolve's")
        }
        guard op != .resolve || (t["clip"] == nil && t["clearFirst"] == nil && t["tiled"] == nil) else {
            throw BenchInvalid("a resolve takes no clip, no clear first and no tiled target")
        }
        var clip: [UInt32]?
        if let value = t["clip"] {
            let c = try counts(value, 4, 0...BenchProtocol.maxSide, "twod.clip")
            guard c[0] < c[2], c[2] <= size.0, c[1] < c[3], c[3] <= size.1 else {
                throw BenchInvalid("twod.clip inside the target")
            }
            clip = c.map(UInt32.init)
        }
        let scale = try byScale(op, t)
        let rop = UInt32(try count(t["rop"] ?? (0 as Any), 0...255, "twod.rop"))
        guard rop == 0 || op != .resolve || scale else {
            throw BenchInvalid("twod.rop: a fill's, a scale's or a by-scale resolve's")
        }
        let invertGrid = try flag("invertGrid", false)
        guard !invertGrid || scale else { throw BenchInvalid("twod.invertGrid: a by-scale resolve's") }
        let grid = BenchProtocol.twodDestination[BenchProtocol.formats[format]!.raw] ?? 0
        guard !invertGrid || BenchProtocol.solidFillFormats.contains(grid) else {
            throw BenchInvalid(
                "twod.invertGrid: the grid is inverted by a fill in its own 2D format, one a solid fill writes")
        }
        return BenchTwod(
            op: op, fmt2d: fmt2d,
            colorFormat: colorFormat,
            color: color.map(UInt32.init), du: try wide("du", 1 << 32), dv: try wide("dv", 1 << 32),
            sx: try wide("sx", 0), sy: try wide("sy", 0), bilinear: try flag("bilinear", false),
            corner: try flag("corner", false), reference: reference, bracketed: try flag("bracketed", true),
            samples: UInt32(samples), clip: clip, clearFirst: try flag("clearFirst", false),
            tiled: try flag("tiled", false), byScale: scale, rop: rop, invertGrid: invertGrid)
    }
}
