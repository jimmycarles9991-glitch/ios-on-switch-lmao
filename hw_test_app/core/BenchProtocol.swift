import CoreFoundation
import Foundation

/// tools/benchloop/protocol.py's batch as the bench loop decodes it: every limit and allowed method range is the
/// same (tools/benchloop/agreement_test.py compares them), so the host's and the app's checks agree.
public enum BenchProtocol {
    public static let version = "benchloop/5"
    /// benchloop/3 is benchloop/4 without storage and the interlock control
    public static let versions = ["benchloop/3", "benchloop/4", version]
    public static let maxExperiments = 256
    public static let maxSide = 2048
    public static let maxDrawCounts = 16
    public static let maxDraws = 256
    public static let maxRepeats = 64
    public static let maxWarmup = 16
    public static let maxCommandBuffers = 1024
    public static let maxCode = 64 * 1024
    public static let maxData = 64 * 1024
    public static let maxMethodPairs = 1024
    public static let maxStorage = 16 * 1024 * 1024
    public static let maxGroups = 65535
    public static let maxThreads = 1024
    public static let sphBytes = 80
    public static let maxCommandBufferMs = 50.0
    static let clockHz = 768e6
    static let lanesPerClock = 256.0

    /// MTLPixelFormat raw value and bytes per texel of each target the raw path renders.
    public static let formats: [String: (raw: UInt32, bytes: Int)] = [
        "r32float": (55, 4),
        "rgba8unorm": (70, 4),
        "bgra8unorm": (80, 4),
        "rgb10a2unorm": (90, 4),
        "rg32float": (105, 8),
        "rgba16float": (115, 8),
        "rgba32float": (125, 16),
        "r8unorm": (10, 1),
        "r16unorm": (20, 2),
        "r16float": (25, 2),
        "rg8unorm": (30, 2),
        "b5g6r5unorm": (40, 2),
        "bgr5a1unorm": (43, 2),
        "rg16unorm": (60, 4),
        "rg16float": (65, 4),
        "rgba8unorm_srgb": (71, 4),
        "bgra8unorm_srgb": (81, 4),
        "rg11b10float": (92, 4),
        "bgr10a2unorm": (94, 4),
        "rgba16unorm": (110, 8),
    ]

    public static let methodRanges: [(UInt32, UInt32)] = [
        (0x02E0, 0x02E4), (0x02F4, 0x02F8), (0x0754, 0x0758), (0x0D9C, 0x0DA0), (0x0DD0, 0x0DD8), (0x0DF0, 0x0DF8),
        (0x0F40, 0x0F54), (0x0F90, 0x0F94), (0x0FAC, 0x0FB0), (0x0FDC, 0x0FE0), (0x10CC, 0x10D0), (0x10E0, 0x10F4),
        (0x113C, 0x1140), (0x1140, 0x1144), (0x1220, 0x1224), (0x1224, 0x1228), (0x12E0, 0x12E8), (0x131C, 0x132C),
        (0x133C, 0x1360),
        (0x1360, 0x1380), (0x13A8, 0x13AC), (0x1480, 0x1500), (0x15B8, 0x15BC), (0x19C0, 0x19C4), (0x1A00, 0x1A20),
        (0x1E00, 0x1F00), (0x2600, 0x2604),
    ]

    /// Every field an experiment and its parts may carry (tools/benchloop/protocol.py EXPERIMENT_KEYS and the rest)
    public static let experimentKeys: Set<String> = [
        "id", "kind", "target", "rect", "draws", "repeats", "warmup", "clear", "readback", "template", "fill",
        "program",
        "compute", "twod", "methods", "restore", "targets", "source", "compare", "compressed", "pack", "storage",
    ]
    public static let targetKeys: Set<String> = ["format", "width", "height"]
    public static let programKeys: Set<String> = ["code", "data", "sph", "gprs", "dataBound"]
    public static let computeKeys: Set<String> = [
        "code", "data", "gprs", "threads", "groups", "output", "chain", "schedule",
    ]
    /// QMDs a compute experiment's own one may name dependent, one after another (protocol.py MAX_CHAIN)
    public static let maxChain = 3
    public static let twodKeys: Set<String> = [
        "op", "fmt2d", "colorFormat", "color", "du", "dv", "sx", "sy", "bilinear", "corner", "reference", "bracketed",
        "samples", "clip", "clearFirst", "tiled", "byScale",
    ]
    public static let sourceKeys: Set<String> = ["format", "width", "height", "data"]
    /// Target formats the 2D engine names a surface format for (raw_probe_ops.h probeTwodFormat)
    public static let twodFormats: Set<UInt32> = [10, 20, 25, 30, 60, 65, 70, 71, 80, 81, 90, 92, 110, 115, 125]
    /// Each of them and that surface format, as probeTwodFormat gives it
    public static let twodDestination: [UInt32: UInt32] = [
        10: 0xF3, 20: 0xEE, 25: 0xF2, 30: 0xEA, 60: 0xDA, 65: 0xDE, 70: 0xD5, 71: 0xD6, 80: 0xCF, 81: 0xD0, 90: 0xD1,
        92: 0xE0, 110: 0xC6, 115: 0xCA, 125: 0xC0,
    ]
    /// The 2D surface formats a solid fill may write, each in its own solid-prim colour format (raw_probe_ops.h
    /// probeSolidFill)
    public static let solidFillFormats: Set<UInt32> = [0xC0, 0xCA, 0xCF, 0xD1, 0xD5, 0xEE, 0xF3]
    /// Target formats the pack has a sampled-quad state for (raw_mip.h mipState): a bilinear scale's 3D compare
    public static let mipFormats: Set<UInt32> = [1, 20, 25, 40, 43, 55, 60, 65, 71, 81, 90, 92, 94, 105, 110, 125]
    /// A multisampled image's samples across and down, log2 (fx_image.cpp msLayout)
    public static let sampleGrids: [Int: (Int, Int)] = [2: (1, 0), 4: (1, 1)]
    public static let picture = (format: "bgra8unorm", width: 64, height: 64)
    public static let compares: [BenchKind: [String]] = [
        .twod: ["program", "copy", "resolve3d", "sampled3d"], .readback: ["plain"],
    ]
    public static let maxOutput = 64 * 1024
    /// A pack experiment's quads and their uniform blocks (protocol.py PACK_KEYS and the rest)
    public static let packKeys: Set<String> = ["form", "ca", "quads"]
    public static let quadKeys: Set<String> = ["rect", "ubos", "vertices", "primitive", "first", "count"]
    public static let uboKeys: Set<String> = ["stage", "slot", "offset", "data"]
    public static let packForms = ["whole", "whole_generic", "split", "split_whole"]
    public static let maxQuads = 64
    public static let maxUbos = 8
    public static let quadRegion = 64 * 1024
    public static let maxVertexBytes = 4096

    public static func methodAllowed(_ method: UInt32) -> Bool {
        method % 4 == 0 && methodRanges.contains { $0.0 <= method && method < $0.1 }
    }

    public static func validName(_ name: String) -> Bool {
        guard let first = name.unicodeScalars.first, name.unicodeScalars.count <= 64 else { return false }
        let lower = { (c: Unicode.Scalar) in ("a"..."z").contains(c) || ("0"..."9").contains(c) }
        return lower(first) && name.unicodeScalars.allSatisfy { lower($0) || $0 == "_" || $0 == "." || $0 == "-" }
    }
}

public enum BenchKind: String, Sendable {
    case noop, known, fragment, compute, readback, twod, pack
}

public enum BenchTwodOp: String, Sendable {
    case fill, scale, resolve
}

/// One 2D-engine operation (hw_test_app/bench/switch_bench.h switch_bench_twod)
public struct BenchTwod: Equatable, Sendable {
    public let op: BenchTwodOp
    public let fmt2d: UInt32
    public let colorFormat: UInt32
    public let color: [UInt32]
    /// 32.32: the source step per destination texel (du, dv) and the source origin (sx, sy)
    public let du: UInt64, dv: UInt64, sx: UInt64, sy: UInt64
    public let bilinear: Bool, corner: Bool
    /// The reference fill (nouveau's EXA solid: SRCCOPY, clip at the surface); `bracketed`: between the waits the
    /// production 2D path puts around 2D work; `samples`: a resolve's source's samples a pixel
    public let reference: Bool, bracketed: Bool
    public let samples: UInt32
    /// The 2D engine's clip rectangle [x0, y0, x1, y1) around the operation; nil: none
    public let clip: [UInt32]?
    /// A clear-only pass on the target just before the operation, in its command buffer (the 11 Nov 15:30 hang's slot)
    public let clearFirst: Bool
    /// A block-linear target (as a private Metal texture is), its texels in and out through the copy engine
    public let tiled: Bool
    /// A resolve read by a scale of the sample grid with this operation's du, dv, sx, sy, bilinear and corner
    public let byScale: Bool
}

/// What a scale, resolve or copy reads: texels the experiment gives, else the app's own picture
public struct BenchSource: Equatable, Sendable {
    public let format: String
    public let formatRaw: UInt32
    public let width: UInt32
    public let height: UInt32
    public let data: Data
}

public enum BenchReadback: String, Sendable {
    case none, summary, full
}

public struct BenchInvalid: Error, Equatable {
    public let why: String

    public init(_ why: String) { self.why = why }
}

public struct BenchProgram: Equatable, Sendable {
    public let sph: Data
    public let code: Data
    public let data: Data
    public let gprs: UInt32
    /// The constant buffer's bound size in bytes (CB_SELECTOR_A); 0: the data padded to 256-byte steps, as NVK binds it
    public let dataBound: UInt32

    public init(sph: Data, code: Data, data: Data, gprs: UInt32, dataBound: UInt32 = 0) {
        (self.sph, self.code, self.data, self.gprs, self.dataBound) = (sph, code, data, gprs, dataBound)
    }
}

public struct BenchExperiment: Equatable, Sendable {
    public let id: String
    public let kind: BenchKind
    public let format: String
    public let formatRaw: UInt32
    public let texelBytes: Int
    public let width: UInt32
    public let height: UInt32
    public let rect: [UInt32]
    public let clear: [Float]
    public let fill: Data?
    public let program: BenchProgram?
    public let threads: [UInt32]
    public let groups: [UInt32]
    public let draws: [UInt32]
    public let repeats: UInt32
    public let warmup: UInt32
    /// (method, value) pairs, flat
    public let methods: [UInt32]
    public let restore: [UInt32]
    public let readback: BenchReadback
    /// Colour 0 bound alone, without the template's other targets (the iterated blend unit takes one target)
    public let singleTarget: Bool
    /// The pack state to draw with, by key hash; 0: the bench's own choice
    public let templateKey: UInt64
    public let twod: BenchTwod?
    public let source: BenchSource?
    /// The same operation again through another unit (BenchProtocol.compares), both results and their difference
    public let compare: String?
    /// A readback's target in compressible memory, its texels in and out through the copy engine
    public let compressed: Bool
    /// Bytes of the buffer a kernel writes, whose address is the first 8 bytes of its shader data; 0: none
    public let outputBytes: Int
    /// The kernels of the QMDs after the experiment's own; only its own is launched (hw_test_app/bench switch_bench.h)
    public var chain: [BenchProgram] = []
    /// Bit i: QMD i (0 the experiment's own) schedules QMD i + 1 when it completes
    public var chainSchedule: UInt32 = 0
    /// A pack experiment's form and quads (its state is templateKey)
    public var pack: BenchPack?
    public var storage: BenchStorage? = nil

    public var commandBuffers: Int { draws.count * Int(repeats + warmup) }
}

/// A zeroed GPU buffer the app allocates for one draw experiment: its address goes into the shader data at `vaAt`, and
/// its words come back with the result
public struct BenchStorage: Equatable, Sendable {
    public let bytes: UInt32
    public let vaAt: UInt32
    public let readback: BenchReadback
}

/// A batch's experiments in order; one that fails its checks is kept with why, so its result says so.
public struct BenchBatch: Sendable {
    public enum Entry: Sendable {
        case experiment(BenchExperiment)
        case invalid(id: String, kind: String, why: String)

        public var id: String {
            switch self {
            case .experiment(let e): return e.id
            case .invalid(let id, _, _): return id
            }
        }
    }

    public let name: String
    public let entries: [Entry]

    public static func decode(_ bytes: Data) throws -> BenchBatch {
        guard let top = try? JSONSerialization.jsonObject(with: bytes) as? [String: Any] else {
            throw BenchInvalid("not a JSON object")
        }
        guard let version = top["protocol"] as? String, BenchProtocol.versions.contains(version) else {
            throw BenchInvalid("protocol is not one of \(BenchProtocol.versions)")
        }
        guard let name = top["batch"] as? String, BenchProtocol.validName(name) else {
            throw BenchInvalid("batch name")
        }
        guard let list = top["experiments"] as? [Any], (1...BenchProtocol.maxExperiments).contains(list.count) else {
            throw BenchInvalid("1...\(BenchProtocol.maxExperiments) experiments a batch")
        }
        var seen = Set<String>()
        let entries = list.enumerated().map { index, item -> Entry in
            let fields = item as? [String: Any] ?? [:]
            var id = fields["id"] as? String ?? ""
            if !BenchProtocol.validName(id) || seen.contains(id) { id = "invalid-\(index)" }
            seen.insert(id)
            do {
                return .experiment(try BenchFields(fields).experiment(id: id))
            } catch let invalid as BenchInvalid {
                return .invalid(id: id, kind: fields["kind"] as? String ?? "-", why: invalid.why)
            } catch {
                return .invalid(id: id, kind: fields["kind"] as? String ?? "-", why: "\(error)")
            }
        }
        return BenchBatch(name: name, entries: entries)
    }
}
