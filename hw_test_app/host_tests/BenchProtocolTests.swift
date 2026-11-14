import Foundation
import HWTestCore
import XCTest

/// The bench loop's decoder against tools/benchloop's golden batch, and the result files the Mac parses.
final class BenchProtocolTests: XCTestCase {
    func golden(_ name: String = "BENCH_GOLDEN") throws -> Data {
        let environment = ProcessInfo.processInfo.environment
        let path = try XCTUnwrap(environment[name])
        let root = environment["TEST_SRCDIR"].map { $0 + "/" } ?? ""
        return try Data(contentsOf: URL(fileURLWithPath: root + path))
    }

    /// protocol.py's pack_test experiment: its quads and blocks decode, a pack names its state, and its result carries
    /// the frontiers its prologues wrote
    func testAPackExperimentDecodesAndItsResultCarriesItsFrontiers() throws {
        let block = Data(repeating: 7, count: 64).base64EncodedString()
        let json = """
            {"protocol": "\(BenchProtocol.version)", "batch": "pack", "experiments": [
              {"id": "iscd", "kind": "pack", "template": "618b45175101cd20", "readback": "full",
               "target": {"format": "rgba16float", "width": 256, "height": 256},
               "pack": {"form": "split_whole", "ca": true, "quads": [
                 {"rect": [0, 0, 128, 256], "ubos": [{"stage": 2, "slot": 1, "offset": 80, "data": "\(block)"}]},
                 {"rect": [128, 0, 256, 256]}]}},
              {"id": "nokey", "kind": "pack", "target": {"format": "rgba16float", "width": 16, "height": 16},
               "pack": {"form": "split", "quads": [{"rect": [0, 0, 16, 16]}]}}]}
            """
        let batch = try BenchBatch.decode(Data(json.utf8))
        guard case .experiment(let e) = batch.entries[0] else { return XCTFail("\(batch.entries[0])") }
        XCTAssertEqual(e.kind, .pack)
        XCTAssertEqual(e.templateKey, 0x618b_4517_5101_cd20)
        XCTAssertEqual(e.pack?.form, "split_whole")
        XCTAssertEqual(e.pack?.ca, true)
        XCTAssertEqual(e.pack?.quads.map(\.rect), [[0, 0, 128, 256], [128, 0, 256, 256]])
        XCTAssertEqual(e.pack?.quads[0].blocks.first?.offset, 80)
        XCTAssertEqual(e.pack?.quads[0].blocks.first?.data.count, 64)
        XCTAssertEqual(e.pack?.quads[1].blocks, [])
        guard case .invalid(_, _, let why) = batch.entries[1] else { return XCTFail("\(batch.entries[1])") }
        XCTAssertTrue(why.contains("template"), why)

        var outcome = BenchOutcome()
        outcome.split = (2, Data([1, 2, 3, 4]))
        let line = BenchResult.encode(batch: "pack", entry: batch.entries[0], outcome: outcome)
        let top = try JSONSerialization.jsonObject(with: line) as? [String: Any]
        let split = top?["split"] as? [String: Any]
        XCTAssertEqual(split?["launches"] as? Int, 2)
        XCTAssertEqual(split?["bytes"] as? Int, 4)
        XCTAssertEqual(split?["frontier"] as? String, Data([1, 2, 3, 4]).base64EncodedString())
    }

    /// tools/benchloop/capture.py's batch of the device's split capture (testdata/split_capture.log): every form's quad
    /// carries the captured vertices and blocks byte for byte, at the residual offset the capture kept
    func testASplitCaptureDecodesToTheBytesItCaptured() throws {
        let batch = try BenchBatch.decode(try golden("BENCH_CAPTURE"))
        let experiments = batch.entries.compactMap { entry -> BenchExperiment? in
            if case .experiment(let e) = entry { return e }
            XCTFail("\(entry.id) did not decode: \(entry)")
            return nil
        }
        XCTAssertEqual(experiments.compactMap(\.pack?.form), BenchProtocol.packForms)
        for e in experiments {
            XCTAssertEqual(e.templateKey, 0x618b_4517_5101_cd20)
            XCTAssertEqual(e.formatRaw, 115)
            let quad = try XCTUnwrap(e.pack?.quads.first)
            XCTAssertEqual(quad.vertices, Data((0..<64).map { UInt8(255 - $0) }))
            XCTAssertEqual([quad.primitive, quad.first, quad.count], [4, 0, 4])
            let block = try XCTUnwrap(quad.blocks.first)
            XCTAssertEqual([block.stage, block.slot, block.offset], [2, 1, 80])
            XCTAssertEqual(block.data, Data(0..<64))
            XCTAssertEqual(quad.blocks.last?.data, Data(repeating: 0x3c, count: 64))
        }
    }

    func testEveryKindOfTheGoldenBatchDecodes() throws {
        let batch = try BenchBatch.decode(try golden())
        XCTAssertEqual(batch.name, "golden")
        let experiments = batch.entries.compactMap { entry -> BenchExperiment? in
            if case .experiment(let e) = entry { return e }
            XCTFail("\(entry.id) did not decode: \(entry)")
            return nil
        }
        XCTAssertEqual(experiments.map(\.kind), [.noop, .known, .fragment, .compute, .readback])
        let fragment = experiments[2]
        XCTAssertEqual(fragment.formatRaw, 125)
        XCTAssertEqual(fragment.rect, [0, 0, 8, 4])
        XCTAssertEqual(fragment.draws, [1, 2, 4])
        XCTAssertEqual(fragment.program?.sph.count, 80)
        XCTAssertEqual(fragment.program?.gprs, 4)
        XCTAssertEqual(fragment.commandBuffers, 21)
        let readback = experiments[4]
        XCTAssertEqual(readback.methods, [0xDD4, 1, 0xDF0, 0, 0xDF4, 0x302A_0000, 0xDD0, 1])
        XCTAssertEqual(readback.restore, [0xDD0, 0, 0xDD4, 0])
        XCTAssertEqual(readback.fill?.count, 4 * 2 * 4)
        XCTAssertEqual(experiments[3].threads, [32, 1, 1])
    }

    func testAnExperimentTheRawPathMayNotTakeIsKeptAsInvalid() throws {
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "b",
            "experiments": [
                [
                    "id": "owned", "kind": "readback", "target": ["format": "bgra8unorm", "width": 4, "height": 4],
                    "program": [
                        "sph": Data(count: 80).base64EncodedString(), "code": Data(count: 32).base64EncodedString(),
                        "gprs": 4,
                    ], "methods": [[0x0200, 1]],
                ],
                ["id": "big", "kind": "noop", "target": ["format": "rgba16float", "width": 4096, "height": 4]],
                ["id": "ok", "kind": "noop", "target": ["format": "rgba16float", "width": 64, "height": 4]],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .invalid(_, _, let why) = decoded.entries[0] else { return XCTFail("owned method accepted") }
        XCTAssertTrue(why.contains("not allowed"), why)
        guard case .invalid = decoded.entries[1] else { return XCTFail("4096 wide accepted") }
        guard case .experiment = decoded.entries[2] else { return XCTFail("a plain noop refused") }
    }

    func testAReadbackMayBindColourZeroAlone() throws {
        let program: [String: Any] = [
            "sph": Data(count: 80).base64EncodedString(), "code": Data(count: 32).base64EncodedString(), "gprs": 4,
        ]
        let target: [String: Any] = ["format": "bgra8unorm_srgb", "width": 4, "height": 4]
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "b",
            "experiments": [
                [
                    "id": "one", "kind": "readback", "target": target, "program": program, "targets": 1,
                    "template": "1b1ae4ebb482e4d1",
                ],
                ["id": "two", "kind": "readback", "target": target, "program": program, "targets": 2],
                ["id": "frag", "kind": "fragment", "target": target, "program": program, "targets": 1],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .experiment(let one) = decoded.entries[0] else { return XCTFail("targets 1 refused") }
        XCTAssertTrue(one.singleTarget)
        XCTAssertEqual(one.formatRaw, 81)
        XCTAssertEqual(one.templateKey, 0x1b1a_e4eb_b482_e4d1)
        guard case .invalid = decoded.entries[1] else { return XCTFail("targets 2 accepted") }
        guard case .invalid = decoded.entries[2] else { return XCTFail("a fragment experiment chose its targets") }
    }

    func testATwodExperimentCarriesItsOperation() throws {
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "b",
            "experiments": [
                [
                    "id": "fill", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8],
                    "rect": [1, 1, 7, 7],
                    "twod": ["op": "fill", "colorFormat": 0xCF, "color": [0xFF00_80FF, 0, 0, 0]],
                ],
                [
                    "id": "half", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8],
                    "twod": ["op": "scale", "du": 1 << 33, "dv": 1 << 33, "bilinear": true],
                ],
                ["id": "none", "kind": "twod", "target": ["format": "bgra8unorm", "width": 8, "height": 8]],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .experiment(let fill) = decoded.entries[0], let f = fill.twod else { return XCTFail("fill refused") }
        XCTAssertEqual([f.colorFormat, f.color[0]], [0xCF, 0xFF00_80FF])
        XCTAssertEqual(f.op, .fill)
        XCTAssertTrue(f.bracketed)
        guard case .experiment(let half) = decoded.entries[1], let h = half.twod else {
            return XCTFail("scale refused")
        }
        XCTAssertEqual(h.du, 1 << 33)
        XCTAssertTrue(h.op == .scale && h.bilinear)
        guard case .invalid = decoded.entries[2] else { return XCTFail("a twod experiment without its operation ran") }
    }

    func testCompeteIsAFlagOfDrawsAndCodeRunsTo128KiB() throws {
        let program = { (bytes: Int) -> [String: Any] in
            [
                "sph": Data(count: 80).base64EncodedString(), "code": Data(count: bytes).base64EncodedString(),
                "gprs": 4,
            ]
        }
        let target: [String: Any] = ["format": "rgba32float", "width": 4, "height": 4]
        let batch: [String: Any] = [
            "protocol": BenchProtocol.version, "batch": "compete",
            "experiments": [
                ["id": "draw", "kind": "fragment", "target": target, "program": program(32), "compete": true],
                ["id": "one", "kind": "fragment", "target": target, "program": program(32), "compete": 1],
                ["id": "noop", "kind": "noop", "target": target, "compete": true],
                ["id": "k128", "kind": "fragment", "target": target, "program": program(128 * 1024)],
                ["id": "past", "kind": "fragment", "target": target, "program": program(128 * 1024 + 32)],
            ],
        ]
        let decoded = try BenchBatch.decode(try JSONSerialization.data(withJSONObject: batch))
        guard case .experiment(let e) = decoded.entries[0] else { return XCTFail("\(decoded.entries[0])") }
        XCTAssertTrue(e.compete)
        guard case .invalid = decoded.entries[1] else { return XCTFail("compete 1 accepted") }
        guard case .invalid = decoded.entries[2] else { return XCTFail("a noop that competes accepted") }
        guard case .experiment(let big) = decoded.entries[3] else { return XCTFail("128 KiB of code refused") }
        XCTAssertFalse(big.compete)
        guard case .invalid = decoded.entries[4] else { return XCTFail("code past 128 KiB accepted") }
        var outcome = BenchOutcome()
        (outcome.competitorBuffers, outcome.competitorWhy) = (12, "")
        let bytes = BenchResult.encode(batch: "compete", entry: decoded.entries[0], outcome: outcome)
        let top = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        XCTAssertEqual((top["competitor"] as? [String: Any])?["buffers"] as? Int, 12)
        let alone = BenchResult.encode(batch: "compete", entry: decoded.entries[3], outcome: outcome)
        XCTAssertNil((try JSONSerialization.jsonObject(with: alone) as? [String: Any])?["competitor"])
    }

    func testResultFileCarriesRunsAndTexels() throws {
        let batch = try BenchBatch.decode(try golden())
        var outcome = BenchOutcome()
        outcome.runs = [(1, [100, 120], [5000, 5100])]
        outcome.texels = Data([1, 2, 3, 4, 1, 2, 3, 4, 9, 9, 9, 9, 1, 2, 3, 4]) + Data(count: 4 * 2 * 4 - 16)
        let bytes = BenchResult.encode(batch: "golden", entry: batch.entries[4], outcome: outcome)
        let top = try XCTUnwrap(JSONSerialization.jsonObject(with: bytes) as? [String: Any])
        XCTAssertEqual(top["protocol"] as? String, BenchProtocol.version)
        XCTAssertEqual(top["status"] as? String, "ok")
        let texels = try XCTUnwrap(top["texels"] as? [String: Any])
        let distinct = try XCTUnwrap(texels["distinct"] as? [[String: Any]])
        XCTAssertEqual(distinct.map { $0["value"] as? String }, ["01020304", "09090909", "00000000"])
        XCTAssertEqual(distinct.map { $0["count"] as? Int }, [3, 1, 4])
        XCTAssertEqual(distinct[1]["first"] as? [Int], [2, 0])
        XCTAssertNotNil(texels["data"])
        let line = BenchResult.logLine(batch: "golden", id: "readback", outcome: outcome)
        XCTAssertTrue(line.hasPrefix("BENCH golden readback ok runs=1:120 crc="), line)
    }

    func testCrcIsZlibs() {
        XCTAssertEqual(BenchResult.crc32(Data("123456789".utf8)), 0xCBF4_3926)
    }

    func testJournalNamesWhatWasRunningWhenTheProcessEnded() {
        let text = "starting a\ndone a ok\nstarting b\n"
        XCTAssertEqual(BenchJournal.crashed(text), ["b"])
        XCTAssertEqual(BenchJournal.finished(text), ["a"])
    }
}
