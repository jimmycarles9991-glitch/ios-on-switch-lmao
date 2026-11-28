import CoreVideo
import Foundation
import HWTestCore
import HWTestKit
import HWTestVideo

/// The scaler's gamut stage through a direct client (video/VICColour.swift), 64x64 sRGB-encoded BGRA surfaces tagged
/// with IOSurfaceSetValue, the 125 lattice inputs first: 709_p3, 709_2020 and p3_2020 each write the scaler's results
/// (video/VICColourReference.swift) within one code (worst_hw_<pair>; an M4 Max's scaler through this same client: 0
/// for all three), alpha 255, status 0; the untagged control is a byte copy (worst_untagged 0). An M4 Max's scaler
/// refuses 64x2 surfaces (kIOReturnBadArgument), hence 64x64. Reported, not judged: the four transfers TegraVIC refuses
/// by design (refused_tf, refused_p3_709, refused_2020, refused_scaled: kIOReturnUnsupported and nothing written is
/// the driver's), which an M4 Max's scaler takes (status 0, written).
struct VICGamutTest: HWTest {
    let id = "gpu.x1.vic_gamut"
    let name = "VIC converts primaries between sRGB-encoded BGRA surfaces within one code of Apple's scaler"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 30 }

    static let keys = [
        "ITU_R_709_2>P3_D65": "709_p3",
        "ITU_R_709_2>ITU_R_2020": "709_2020",
        "P3_D65>ITU_R_2020": "p3_2020",
    ]

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .success(let scaler) = ScalerClient.open() else { return .fail("no IOSurfaceAccelerator client") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            for pair in VICColourReference.pairs {
                let key = Self.keys["\(pair.from)>\(pair.to)"]!, g = try VICColour.gamut(scaler, pair)
                metrics.set("status_" + key, Int(g.status))
                metrics.set("worst_hw_" + key, g.worst)
                metrics.set("alpha_" + key, g.alpha)
                if g.worst > 1 { metrics.set("worst3_" + key, g.worstSamples) }
                checks.append((
                    "\(key): status \(g.status) worst \(g.worst) alpha \(g.alpha)",
                    g.status == 0 && g.worst <= 1 && g.alpha == 255
                ))
            }
            let (status, worst) = try VICColour.copy(scaler, from: [:], to: [:])
            metrics.set("status_untagged", Int(status))
            metrics.set("worst_untagged", worst)
            checks.append(("untagged: status \(status) worst \(worst)", status == 0 && worst == 0))
            let refusals: [(String, [String: String], [String: String], Int)] = [
                ("refused_tf", VICColour.tags("ITU_R_709_2", VICColour.tf709), VICColour.tags("ITU_R_709_2"), 64),
                ("refused_p3_709", VICColour.tags("P3_D65"), VICColour.tags("ITU_R_709_2"), 64),
                ("refused_2020", VICColour.tags("ITU_R_2020"), VICColour.tags("ITU_R_709_2"), 64),
                ("refused_scaled", VICColour.tags("ITU_R_709_2"), VICColour.tags("P3_D65"), 32),
            ]
            for (key, from, to, size) in refusals {
                let (status, changed) = try VICColour.refused(scaler, from: from, to: to, size: size)
                metrics.set(key, "0x" + String(UInt32(bitPattern: status), radix: 16) + (changed ? "_written" : ""))
            }
        } catch {
            checks.append(("\(error)", false))
        }
        return .judged(checks, metrics)
    }
}

/// The colour tags a direct scaler client sets reach the scaler (the kernel reads IOSurface's attachments), each
/// against an M4 Max's scaler through the same client: a '420v' of luma 63 and chroma (102, 240) into BGRA reads BT.709
/// with IOSurfaceYCbCrMatrix ITU_R_709_2 (tag_709_red 255, tag_709_green 1; judged 254-255 and 0-1) and BT.601 untagged
/// (tag_none_red 233; judged 233-234); with IOSurfaceChromaLocationTopField TopLeft it converts (status_chroma 0); a
/// BGRA copy from SMPTE_C or EBU_3213 primaries to ITU_R_709_2, transfer ITU_R_709_2 both, is a byte copy
/// (worst_smptec_copy, worst_ebu_copy 0); SMPTE_C to P3_D65, sRGB both, converts as ITU_R_709_2 does (worst_smptec_p3
/// against the 709 -> P3 results: 0 on the M4 Max, judged at most 1). Reported: status_chroma_mixed (top Left, bottom
/// Center), which TegraVIC refuses (kIOReturnUnsupported) and an M4 Max's scaler converts.
struct VICTagsTest: HWTest {
    let id = "gpu.x1.vic_tags"
    let name = "The scaler reads a direct client's matrix, siting and primaries tags as Apple's scaler does"
    let subsystem = Subsystem.images
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        guard case .success(let scaler) = ScalerClient.open() else { return .fail("no IOSurfaceAccelerator client") }
        var metrics = Metrics()
        var checks: [(String, Bool)] = []
        do {
            let (s709, red709, green709) = try VICColour.ycbcr(scaler, VICColour.matrixTag("ITU_R_709_2"))
            metrics.set("tag_709_red", red709)
            metrics.set("tag_709_green", green709)
            checks.append((
                "tag_709: status \(s709) red \(red709) green \(green709)",
                s709 == 0 && red709 >= 254 && green709 <= 1
            ))
            let (sNone, redNone, _) = try VICColour.ycbcr(scaler, [:])
            metrics.set("tag_none_red", redNone)
            checks.append(("tag_none: status \(sNone) red \(redNone)", sNone == 0 && (233 ... 234).contains(redNone)))
            let (chroma, _, _) = try VICColour.ycbcr(scaler, ["IOSurfaceChromaLocationTopField": "TopLeft"])
            metrics.set("status_chroma", Int(chroma))
            checks.append(("status_chroma=\(chroma)", chroma == 0))
            let (mixed, _, _) = try VICColour.ycbcr(
                scaler, ["IOSurfaceChromaLocationTopField": "Left", "IOSurfaceChromaLocationBottomField": "Center"]
            )
            metrics.set("status_chroma_mixed", "0x" + String(UInt32(bitPattern: mixed), radix: 16))
            for (key, primaries) in [("smptec", "SMPTE_C"), ("ebu", "EBU_3213")] {
                let (status, worst) = try VICColour.copy(
                    scaler, from: VICColour.tags(primaries, VICColour.tf709), to: VICColour.tags(
                        "ITU_R_709_2",
                        VICColour.tf709
                    )
                )
                metrics.set("status_\(key)_copy", Int(status))
                metrics.set("worst_\(key)_copy", worst)
                checks.append(("\(key)_copy: status \(status) worst \(worst)", status == 0 && worst == 0))
            }
            let g = try VICColour.gamut(scaler, VICColourReference.pairs[0], from: "SMPTE_C")
            metrics.set("status_smptec_p3", Int(g.status))
            metrics.set("worst_smptec_p3", g.worst)
            checks.append(("smptec_p3: status \(g.status) worst \(g.worst)", g.status == 0 && g.worst <= 1))
        } catch {
            checks.append(("\(error)", false))
        }
        return .judged(checks, metrics)
    }
}
