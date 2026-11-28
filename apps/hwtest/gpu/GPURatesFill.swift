import Foundation
import HWTestCore
import Metal

/// gpu.perf.fill: metal.benchmark's fill (16 opaque full-screen layers into a 1920x1080 private BGRA8 target) across
/// what could have halved it between the 24 and 25 Nov builds (11.5 to 5.3 Gpix/s), each cell `fill_<target>_<memory>_
/// <bands>_gpix_s`:
/// - target: `fresh`, a target made just before its one timed run, as metal.benchmark draws it; `plain`, made on plain
///   memory (switchMetalProbePlainPrivate:) and timed warm, the fastest of `repeats`; `comp`, settled on compressed
///   memory (CompressionSettled.remade, passed in) and timed warm.
/// - memory: `smmu`, the kernel's choice, which since 72a0e9b01 maps an uncompressed BO of 64 KiB or more through the
///   Tegra SMMU's 64 KiB big pages; `direct`, the BO allocated with TegraGM20BBoProbeDirect (switchMetalProbeDirectPa:),
///   on direct physical addresses as before it. Compressed memory takes neither.
/// - bands: `b0`, the facade's band budget at 0; `prod`, production's, as metal.benchmark runs.
/// Proof: `_bos_smmu` and `_bos_direct` are how many BOs the target's creation made that the kernel mapped through the
/// SMMU and that carried the probe flag; `_comptags` and `_kind` the target's facts after its runs. `lane_mhz_first`
/// and `lane_mhz_last` bracket the cells with the FMA clock, so a clock change is not read as a memory one. NOTES has
/// the table of which differing pair names which cause.
extension GPURates {
    public static func fill(
        _ gpu: GPU, _ s: Sizes = .device, productionBudget: Int?,
        settleCompression: (() async throws -> Void)? = nil
    ) async throws -> SceneResult {
        let b = GPURates(gpu, s)
        b.settleCompression = settleCompression
        var notes: [String] = []
        try await b.alu()
        b.metrics.set("lane_mhz_first", b.laneMhz, digits: 1)
        let probed = b.has("switchMetalProbeDirectPa:")
        if !probed { notes.append("the device has no switchMetalProbeDirectPa: probe: no direct cells") }
        let budgets: [(String, Int?)] = [("b0", 0), ("prod", productionBudget)]
        for (bands, budget) in budgets {
            guard let budget else { continue }
            b.ask(gpu.device, "switchMetalBandBudget:", NSNumber(value: budget))
            defer { b.ask(gpu.device, "switchMetalBandBudget:", NSNumber(value: 0)) }
            for memory in probed ? ["smmu", "direct"] : ["smmu"] {
                await b.isolated("fill_fresh_\(memory)_\(bands)") { try await b.fillFresh(memory, bands) }
                await b.isolated("fill_plain_\(memory)_\(bands)") { try await b.fillWarm("plain", memory, bands) }
            }
            await b.isolated("fill_comp_\(bands)") { try await b.fillWarm("comp", "smmu", bands) }
        }
        try await b.alu()
        b.metrics.set("lane_mhz_last", b.laneMhz, digits: 1)
        var r = SceneResult()
        r.metrics = b.metrics
        r.notes = notes
        for p in b.problems { r.expect(false, p) }
        return r
    }

    static let fillLayers = 16

    func fillEncode(_ target: MTLTexture, _ solid: MTLRenderPipelineState) -> (MTLCommandBuffer) -> Void {
        { c in
            let e = self.pass(target, nil, c)
            e.setRenderPipelineState(solid)
            for i in 0 ..< Self.fillLayers {
                var k = SIMD4<Float>(Float(i) / Float(Self.fillLayers), 0.5, 0.25, 1)
                e.setFragmentBytes(&k, length: 16, index: 0)
                self.quad(e, z: 0.5)
            }
            e.endEncoding()
        }
    }

    func fillRate(_ ms: Double) -> Double {
        Self.rate(Double(s.fillWidth * s.fillHeight * Self.fillLayers), ms) / 1e9
    }

    /// The target `make` makes with the probe flag on its BOs where `memory` is "direct", and how many BOs the kernel
    /// mapped through the SMMU and took the flag while it was made
    func madeUnder(_ memory: String, _ cell: String, _ make: () async throws -> MTLTexture) async throws -> MTLTexture {
        probe("switchMetalProbeDirectPa:", memory == "direct" ? 1 : 0)
        defer { probe("switchMetalProbeDirectPa:", 0) }
        var made: MTLTexture?
        let grew = try await grown(["bos_smmu", "bos_probe_direct"]) { made = try await make() }
        metrics.set("\(cell)_bos_smmu", grew["bos_smmu"] ?? 0)
        metrics.set("\(cell)_bos_direct", grew["bos_probe_direct"] ?? 0)
        if memory == "direct" && has("switchMetalProbeDirectPa:") && (grew["bos_probe_direct"] ?? 0) == 0 {
            problems.append("\(cell): no BO of the target carried the probe flag")
        }
        guard let made else { throw TestFailure("\(cell): no target") }
        return made
    }

    func fillFacts(_ cell: String, _ target: MTLTexture) {
        let f = facts(target)
        metrics.set("\(cell)_comptags", f["comptags"] ?? 0)
        metrics.set("\(cell)_kind", "0x" + String(f["kind"] ?? 0, radix: 16))
    }

    /// metal.benchmark's draw: one timed run into a target made just before it
    func fillFresh(_ memory: String, _ bands: String) async throws {
        let cell = "fill_fresh_\(memory)_\(bands)"
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let target = try await madeUnder(memory, cell) {
            try self.gpu.texture(.bgra8Unorm, self.s.fillWidth, self.s.fillHeight)
        }
        let t = try await gpu.run(fillEncode(target, solid))
        guard t.gpuTimed else { throw TestFailure("\(cell): no GPU timestamps") }
        metrics.set("\(cell)_gpix_s", fillRate(t.gpuMilliseconds), digits: 2)
        fillFacts(cell, target)
    }

    /// The fastest of `repeats` runs after a warm one, into a plain or a settled compressed target
    func fillWarm(_ target: String, _ memory: String, _ bands: String) async throws {
        let cell = target == "comp" ? "fill_comp_\(bands)" : "fill_\(target)_\(memory)_\(bands)"
        let solid = try gpu.render(fragment: "solid_fragment", format: .bgra8Unorm)
        let make = { try self.gpu.texture(.bgra8Unorm, self.s.fillWidth, self.s.fillHeight) }
        let made: MTLTexture
        if target == "comp" {
            made = try await madeUnder(memory, cell) {
                let first = try make()
                guard self.facts(first)["comptags"] == 0, let settle = self.settleCompression else { return first }
                try await settle()
                return try make()
            }
        } else {
            probe("switchMetalProbePlainPrivate:", 1)
            defer { probe("switchMetalProbePlainPrivate:", 0) }
            made = try await madeUnder(memory, cell, make)
        }
        let ms = try await fastest(cell, fillEncode(made, solid))
        metrics.set("\(cell)_gpix_s", fillRate(ms), digits: 2)
        fillFacts(cell, made)
        let f = facts(made)
        if target == "comp" && f["compression"] == 1 && f["comptags"] != 1 {
            problems.append("\(cell): the target ran on plain memory, none compressed was made")
        }
        if target == "plain" && f["comptags"] == 1 {
            problems.append("\(cell): the plain target moved to compressed memory")
        }
    }
}
