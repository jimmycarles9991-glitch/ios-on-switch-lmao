import Foundation
import HWTestCore
import XCTest

/// The bench's later forms as the app decodes them: strict fields, compute output, sources, resolves, compressed
/// targets and compare forms, and the difference histogram their results carry.
final class BenchFormsTests: XCTestCase {
    let program: [String: Any] = [
        "sph": Data(count: 80).base64EncodedString(), "code": Data(count: 32).base64EncodedString(), "gprs": 4,
    ]

    func decode(_ experiments: [[String: Any]]) throws -> [BenchBatch.Entry] {
        let batch: [String: Any] = ["protocol": BenchProtocol.version, "batch": "b", "experiments": experiments]
        return try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch)).entries
    }

    func why(_ entry: BenchBatch.Entry) -> String? {
        if case .invalid(_, _, let why) = entry { return why }
        return nil
    }

    func target(_ format: String, _ width: Int, _ height: Int) -> [String: Any] {
        ["format": format, "width": width, "height": height]
    }

    /// tools/benchloop:generated_batches: every generated command's batches as the device receives them
    func testEveryGeneratedBatchDecodesWithNoInvalidExperiment() throws {
        let environment = ProcessInfo.processInfo.environment
        let path = try XCTUnwrap(environment["BENCH_GENERATED"])
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        let data = try Data(contentsOf: URL(fileURLWithPath: root + path))
        let batches = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [[String: Any]])
        XCTAssertGreaterThan(batches.count, 5)
        var experiments = 0
        for batch in batches {
            let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
            for entry in decoded.entries {
                experiments += 1
                if let why = why(entry) { XCTFail("\(decoded.name) \(entry.id): \(why)") }
            }
        }
        XCTAssertGreaterThan(experiments, 300)
    }

    func testAnUnknownFieldIsAnInvalidExperiment() throws {
        let entries = try decode([
            ["id": "a", "kind": "noop", "target": target("bgra8unorm", 4, 4), "colour": [0, 0, 0, 0]],
            ["id": "b", "kind": "twod", "target": target("bgra8unorm", 4, 4), "twod": ["op": "fill", "scissor": 1]],
            ["id": "c", "kind": "noop", "target": ["format": "bgra8unorm", "width": 4, "height": 4, "depth": 1]],
        ])
        XCTAssertEqual(entries.map { why($0)?.contains("unknown field") }, [true, true, true])
    }

    func testAKernelOutputNeedsRoomForItsAddress() throws {
        let kernel: [String: Any] = [
            "code": Data(count: 32).base64EncodedString(), "data": Data(count: 16).base64EncodedString(), "gprs": 4,
            "threads": [32, 1, 1], "groups": [1, 1, 1], "output": 128,
        ]
        var short = kernel
        short["data"] = ""
        let entries = try decode([
            ["id": "out", "kind": "compute", "target": target("rgba32float", 1, 1), "compute": kernel],
            ["id": "short", "kind": "compute", "target": target("rgba32float", 1, 1), "compute": short],
        ])
        guard case .experiment(let out) = entries[0] else { return XCTFail(why(entries[0]) ?? "") }
        XCTAssertEqual(out.outputBytes, 128)
        XCTAssertTrue(why(entries[1])?.contains("address") == true)
    }

    func testAResolveCoversItsSampleGridFromTheSource() throws {
        let source: [String: Any] = [
            "format": "rgba8unorm", "width": 16, "height": 16, "data": Data(count: 16 * 16 * 4).base64EncodedString(),
        ]
        let entries = try decode([
            [
                "id": "pic", "kind": "twod", "target": target("bgra8unorm", 32, 32),
                "twod": ["op": "resolve", "samples": 4],
                "compare": "resolve3d",
            ],
            [
                "id": "own", "kind": "twod", "target": target("rgba8unorm", 8, 16), "source": source,
                "twod": ["op": "resolve", "samples": 2],
            ],
            [
                "id": "big", "kind": "twod", "target": target("bgra8unorm", 33, 32),
                "twod": ["op": "resolve", "samples": 4],
            ],
            [
                "id": "part", "kind": "twod", "target": target("bgra8unorm", 8, 8), "rect": [0, 0, 4, 4],
                "twod": ["op": "resolve", "samples": 4],
            ],
            [
                "id": "three", "kind": "twod", "target": target("bgra8unorm", 8, 8),
                "twod": ["op": "resolve", "samples": 3],
            ],
        ])
        guard case .experiment(let pic) = entries[0], let p = pic.twod else { return XCTFail(why(entries[0]) ?? "") }
        XCTAssertEqual([p.op == .resolve, pic.compare == "resolve3d"], [true, true])
        XCTAssertEqual(p.samples, 4)
        guard case .experiment(let own) = entries[1] else { return XCTFail(why(entries[1]) ?? "") }
        XCTAssertEqual(own.source?.formatRaw, 70)
        XCTAssertEqual(own.source?.data.count, 16 * 16 * 4)
        XCTAssertNotNil(why(entries[2]))
        XCTAssertNotNil(why(entries[3]))
        XCTAssertNotNil(why(entries[4]))
    }

    func testCompareFormsNeedTheirOperation() throws {
        let entries = try decode([
            [
                "id": "copy", "kind": "twod", "target": target("bgra8unorm", 16, 16), "twod": ["op": "scale"],
                "compare": "copy",
            ],
            [
                "id": "copy-format", "kind": "twod", "target": target("rgba8unorm", 16, 16), "twod": ["op": "scale"],
                "compare": "copy",
            ],
            [
                "id": "prog", "kind": "twod", "target": target("bgra8unorm", 16, 16),
                "twod": ["op": "fill", "colorFormat": 0xCF, "reference": true, "bracketed": false],
                "compare": "program", "program": program,
            ],
            [
                "id": "prog-none", "kind": "twod", "target": target("bgra8unorm", 16, 16), "twod": ["op": "fill"],
                "compare": "program",
            ],
            [
                "id": "stray", "kind": "twod", "target": target("bgra8unorm", 16, 16), "twod": ["op": "fill"],
                "program": program,
            ],
            [
                "id": "plain", "kind": "readback", "target": target("bgra8unorm", 16, 16), "program": program,
                "compressed": true, "compare": "plain",
            ],
            [
                "id": "plain-alone", "kind": "readback", "target": target("bgra8unorm", 16, 16), "program": program,
                "compare": "plain",
            ],
            [
                "id": "ref-scale", "kind": "twod", "target": target("bgra8unorm", 16, 16),
                "twod": ["op": "scale", "reference": true],
            ],
        ])
        XCTAssertNil(why(entries[0]))
        XCTAssertNotNil(why(entries[1]))
        guard case .experiment(let prog) = entries[2], let t = prog.twod else { return XCTFail(why(entries[2]) ?? "") }
        XCTAssertEqual([t.reference, t.bracketed], [true, false])
        XCTAssertNotNil(prog.program)
        XCTAssertNotNil(why(entries[3]))
        XCTAssertNotNil(why(entries[4]))
        guard case .experiment(let plain) = entries[5] else { return XCTFail(why(entries[5]) ?? "") }
        XCTAssertTrue(plain.compressed)
        XCTAssertNotNil(why(entries[6]))
        XCTAssertNotNil(why(entries[7]))
    }

    func testAFillTakesOnlyItsDestinationsOwnFormat() throws {
        let fill = { (id: String, format: String, twod: [String: Any]) -> [String: Any] in
            let op = (["op": "fill"] as [String: Any]).merging(twod) { $1 }
            return ["id": id, "kind": "twod", "target": self.target(format, 16, 16), "twod": op]
        }
        let entries = try decode([
            fill("own", "bgra8unorm", ["colorFormat": 0xCF]),
            fill("floats-bgra8", "bgra8unorm", ["colorFormat": 0xC0]),
            fill("floats-f32", "rgba32float", ["colorFormat": 0xC0]),
            fill("srgb", "bgra8unorm_srgb", ["colorFormat": 0xCF]),
            fill("srgb-unorm", "bgra8unorm_srgb", ["colorFormat": 0xCF, "fmt2d": 0xCF]),
            fill("rg8", "rg8unorm", ["colorFormat": 0xEA]),
        ])
        XCTAssertEqual(entries.map { why($0) == nil }, [true, false, true, false, true, false])
    }

    func testClipClearFirstAndTheSampledCompareNeedTheirShapes() throws {
        let entries = try decode([
            [
                "id": "clip", "kind": "twod", "target": target("bgra8unorm", 16, 16),
                "twod": ["op": "fill", "colorFormat": 0xCF, "clip": [4, 4, 12, 10], "clearFirst": true],
            ],
            [
                "id": "clip-out", "kind": "twod", "target": target("bgra8unorm", 16, 16),
                "twod": ["op": "fill", "clip": [4, 4, 17, 10]],
            ],
            [
                "id": "resolve-clear", "kind": "twod", "target": target("bgra8unorm", 8, 8),
                "twod": ["op": "resolve", "samples": 4, "tiled": true],
            ],
            [
                "id": "s3d", "kind": "twod", "target": target("rgb10a2unorm", 32, 32),
                "twod": ["op": "scale", "du": 1 << 33, "dv": 1 << 33, "bilinear": true], "compare": "sampled3d",
            ],
            [
                "id": "s3d-bgra", "kind": "twod", "target": target("bgra8unorm", 32, 32),
                "twod": ["op": "scale", "du": 1 << 33, "dv": 1 << 33, "bilinear": true], "compare": "sampled3d",
            ],
            [
                "id": "s3d-corner", "kind": "twod", "target": target("rgb10a2unorm", 32, 32),
                "twod": ["op": "scale", "bilinear": true, "corner": true], "compare": "sampled3d",
            ],
        ])
        guard case .experiment(let clip) = entries[0], let t = clip.twod else { return XCTFail(why(entries[0]) ?? "") }
        XCTAssertEqual(t.clip, [4, 4, 12, 10])
        XCTAssertTrue(t.clearFirst)
        XCTAssertFalse(t.tiled)
        XCTAssertNotNil(why(entries[1]))
        XCTAssertNotNil(why(entries[2]))
        XCTAssertNil(why(entries[3]))
        XCTAssertTrue(why(entries[4])?.contains("sampled-quad") == true)
        XCTAssertNotNil(why(entries[5]))
    }

    func testAResolveMayBeReadByAScaleAndComparedWithAProgram() throws {
        let entries = try decode([
            [
                "id": "sweep", "kind": "twod", "target": target("bgra8unorm", 32, 32),
                "twod": ["op": "resolve", "samples": 4, "byScale": true, "du": 1 << 33, "sx": 1 << 28, "corner": true],
            ],
            [
                "id": "vsprog", "kind": "twod", "target": target("bgra8unorm", 32, 32),
                "twod": ["op": "resolve", "samples": 4], "compare": "program", "program": program,
            ],
            [
                "id": "fill-byscale", "kind": "twod", "target": target("bgra8unorm", 16, 16),
                "twod": ["op": "fill", "byScale": true],
            ],
        ])
        guard case .experiment(let sweep) = entries[0], let t = sweep.twod else {
            return XCTFail(why(entries[0]) ?? "")
        }
        XCTAssertTrue(t.byScale && t.corner)
        XCTAssertEqual(t.sx, 1 << 28)
        XCTAssertNil(why(entries[1]))
        XCTAssertNotNil(why(entries[2]))
    }

    func testTheHistogramCountsEachTexelsLargestChannelDifference() {
        let a = Data([10, 20, 30, 40, 0, 0, 0, 0, 255, 255, 255, 255])
        let b = Data([10, 21, 30, 38, 0, 0, 0, 0, 0, 255, 255, 255])
        XCTAssertEqual(
            BenchDifference.histogram(a, b, format: "bgra8unorm", texelBytes: 4), ["2": 1, "0": 1, ">64": 1])
        let ten = { (r: UInt32, a: UInt32) -> Data in withUnsafeBytes(of: (r | a << 30).littleEndian) { Data($0) } }
        XCTAssertEqual(
            BenchDifference.histogram(ten(1023, 3), ten(1020, 2), format: "rgb10a2unorm", texelBytes: 4), ["3": 1])
        XCTAssertEqual(BenchDifference.histogram(a, b.prefix(8), format: "bgra8unorm", texelBytes: 4), [:])
    }

    func testAResultCarriesItsOutputItsCompareAndItsForm() throws {
        let kernel: [String: Any] = [
            "code": Data(count: 32).base64EncodedString(), "data": Data(count: 16).base64EncodedString(), "gprs": 4,
            "threads": [4, 1, 1], "groups": [1, 1, 1], "output": 16,
        ]
        let entries = try decode([
            [
                "id": "out", "kind": "compute", "target": target("rgba32float", 1, 1), "compute": kernel,
                "readback": "full",
            ],
            [
                "id": "copy", "kind": "twod", "target": target("bgra8unorm", 2, 1), "twod": ["op": "scale"],
                "compare": "copy", "readback": "full",
            ],
        ])
        var outcome = BenchOutcome()
        outcome.output = Data([0, 0, 0, 0xB0, 1, 0, 0, 0xB0, 2, 0, 0, 0xB0, 3, 0, 0, 0xB0])
        let out = try XCTUnwrap(
            JSONSerialization.jsonObject(with: BenchResult.encode(batch: "b", entry: entries[0], outcome: outcome))
                as? [String: Any])
        let output = try XCTUnwrap(out["output"] as? [String: Any])
        XCTAssertEqual(output["bytes"] as? Int, 16)
        XCTAssertEqual((output["distinct"] as? [[String: Any]])?.count, 4)
        var twod = BenchOutcome()
        twod.texels = Data([1, 2, 3, 4, 5, 6, 7, 8])
        twod.compareTexels = Data([1, 2, 3, 4, 5, 6, 7, 9])
        twod.form = ["bracketed": 1, "samples": 1]
        let top = try XCTUnwrap(
            JSONSerialization.jsonObject(with: BenchResult.encode(batch: "b", entry: entries[1], outcome: twod))
                as? [String: Any])
        let compare = try XCTUnwrap(top["compare"] as? [String: Any])
        XCTAssertEqual(compare["histogram"] as? [String: Int], ["0": 1, "1": 1])
        XCTAssertEqual((top["form"] as? [String: Int])?["bracketed"], 1)
        var failed = BenchOutcome()
        failed.texels = twod.texels
        failed.compareFailure = ("refused", "the pack has no resolve state for the format")
        let refused = try XCTUnwrap(
            JSONSerialization.jsonObject(with: BenchResult.encode(batch: "b", entry: entries[1], outcome: failed))
                as? [String: Any])
        XCTAssertEqual((refused["compare"] as? [String: Any])?["status"] as? String, "refused")
    }
}
