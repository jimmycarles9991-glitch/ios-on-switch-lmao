import Foundation
import HWTestCore
import HWTestGPU
import Metal

/// replaceRegion and getBytes of a 500 8x8 over a '420v' of rows 8 at offset 8 in 8 MiB + 16 KiB, every byte 0xee
/// (gpu/YcbcrCpuCopies.swift), against an M4 Max under macOS 27.2.1 (this scene, three runs): a write of (2,3) 4x4 at
/// bytesPerRow 16 lands its codes at 8 + (3 + y) * 8 + 2 + x and nothing else (ycbcr_cpu_write_off 0); a read of the
/// region gives the luma bytes there, the rest of the buffer as it was (ycbcr_cpu_read_off 0); a 508 over a '420v'
/// moves nothing either way (ycbcr_cpu_packed_moved 0); a write at bytesPerRow 2, under the 4-byte row, moves nothing
/// (ycbcr_cpu_short_rows_moved 0) and the driver logs one gm20b:texture.cpu.unreached line for it
/// (ycbcr_cpu_unreached_lines 1).
struct YcbcrCpuCopiesTest: HWTest {
    let id = "gpu.gm20b.ycbcr_cpu_copies"
    let name = "CPU copies of YCbCr textures move the luma plane alone at its own rows, as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    static let line = "with bytesPerRow 2 under its 4-byte row"

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            let r = try YcbcrCpuCopies.observe(gpu)
            metrics.set("ycbcr_cpu_made", r.made)
            guard r.made else { return [("no 500 over a '420v' of rows 8", false)] }
            let counts = [
                ("ycbcr_cpu_write_off", r.writeOff), ("ycbcr_cpu_read_off", r.readOff),
                ("ycbcr_cpu_packed_moved", r.packedMoved), ("ycbcr_cpu_short_rows_moved", r.shortRowsMoved),
            ]
            for (key, value) in counts { metrics.set(key, value) }
            let lines = PlanesBlitsTest.refusalLines("texture.cpu.unreached")?.filter { $0.contains(Self.line) }
            metrics.set("ycbcr_cpu_unreached_lines", lines?.count ?? -1)
            return counts.map { ("\($0.0)=\($0.1)", $0.1 == 0) } + [
                ("a 508 over a '420v'", r.packedMade),
                ("one gm20b:texture.cpu.unreached line (\(lines?.count ?? -1))", lines?.count == 1),
            ]
        }
    }
}

/// Texture buffers of 500, 503 and 540 over a '420v' and of 588 and 589 over a 'p420' (gpu/YcbcrTexelBuffers.swift), 64
/// texels over a 64x8 of rows 256 and 16384 over two rows, each against the 2D texture of its format over the same
/// plane: an M4 Max under macOS 27.2.1 makes every one, a kernel sees its width and reads texel i as the 2D texture's
/// (i, 0) (this scene, three runs, 0 texels off; the ytread probe agrees at 64). ycbcr_tbuf_made (of 10),
/// ycbcr_tbuf_row_off_<format>_<width> (texels further than half a code from the 2D texture's).
struct YcbcrTexelBuffersTest: HWTest {
    let id = "gpu.gm20b.ycbcr_texel_buffers"
    let name = "YCbCr and packed 10-bit texture buffers up to 16384 texels read as their 2D textures' rows, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            var made = 0
            for format in YcbcrTexelBuffers.formats {
                for width in [64, 16384] {
                    let key = "\(format)_\(width)"
                    do {
                        let r = try await YcbcrTexelBuffers.row(gpu, format, width: width)
                        metrics.set("ycbcr_tbuf_row_off_" + key, r.made ? r.off : -1)
                        guard r.made else {
                            checks.append(("\(key): no texture buffer", false))
                            continue
                        }
                        made += 1
                        checks += [("\(key): width \(r.width)", r.width == width), ("\(key): \(r.off) off", r.off == 0)]
                    } catch {
                        metrics.set("ycbcr_tbuf_row_off_" + key, -1)
                        checks.append(("\(key): \(GPU.describe(error))", false))
                    }
                }
            }
            metrics.set("ycbcr_tbuf_made", made)
            return checks
        }
    }
}

/// Texture buffers of 20000 texels of the same five formats over planes of two rows (gpu/YcbcrTexelBuffers.swift wide),
/// against the 2D texture of 16384 x 2 over the same plane, and a 500 texture buffer of render-target usage. An M4 Max
/// under macOS 27.2.1 (this scene, three runs) makes every wide one, a kernel sees 20000 and reads texel i as the 2D
/// texture's (i % 16384, i / 16384): past texel 16383 the plane's second row (ycbcr_tbuf_wide_off_<format> 0); it makes
/// no texture buffer of 500 of render-target usage (ycbcr_tbuf_render_made 0).
struct YcbcrTexelBufferWideTest: HWTest {
    let id = "gpu.gm20b.ycbcr_texel_buffer_wide"
    let name = "YCbCr and packed texture buffers past 16384 texels read the next row on, as Metal does, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for format in YcbcrTexelBuffers.formats {
                do {
                    let r = try await YcbcrTexelBuffers.wide(gpu, format)
                    metrics.set("ycbcr_tbuf_wide_off_\(format)", r.made ? r.off : -1)
                    guard r.made else {
                        checks.append(("\(format): no texture buffer of 20000", false))
                        continue
                    }
                    checks.append(("\(format): width \(r.width)", r.width == 20000))
                    checks.append(("\(format): \(r.off) off", r.off == 0))
                } catch {
                    metrics.set("ycbcr_tbuf_wide_off_\(format)", -1)
                    checks.append(("\(format): \(GPU.describe(error))", false))
                }
            }
            do {
                let made = try YcbcrTexelBuffers.renderUsageMade(gpu)
                metrics.set("ycbcr_tbuf_render_made", made)
                checks.append(("a 500 texture buffer of render-target usage is nil", !made))
            } catch {
                checks.append(("render usage: \(GPU.describe(error))", false))
            }
            return checks
        }
    }
}

/// R10Unorm_PACKED 32x16 and RG10Unorm_PACKED 16x8 of three levels over a 'p420' (gpu/PackedLevels.swift), against an
/// M4 Max under macOS 27.2.1 (this scene, three runs): made, a kernel sees 3 levels each half the last
/// (packed_<format>_levels_wrong, _dims_wrong 0); getBytes of each level writes nothing (_getbytes_written 0),
/// replaceRegion of levels 0 and 1 and generateMipmaps change no surface byte (_replace_changed, _generate_changed 0).
/// Reported, not judged: texels of every level read and sampled off level 0's (_read_wrong, _sample_wrong), which an M4
/// Max reads exactly in some runs and displaced in others, and level 1's red code under a linear-minifying sampler
/// (_minmag_sample_code).
struct PackedLevelsTest: HWTest {
    let id = "gpu.gm20b.packed_levels"
    let name = "Packed 10-bit plane textures of three levels show their levels and take no CPU or mipmap writes, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for c in PackedLevels.cases {
                let key = "packed_\(c.format)"
                do {
                    let r = try await PackedLevels.observe(gpu, c)
                    metrics.set(key + "_made", r.made)
                    guard r.made else {
                        checks.append(("\(c.format): no texture of three levels", false))
                        continue
                    }
                    let judged = [
                        ("_levels_wrong", r.levelsWrong), ("_dims_wrong", r.dimsWrong),
                        ("_getbytes_written", r.getBytesWritten), ("_replace_changed", r.replaceChanged),
                        ("_generate_changed", r.generateChanged),
                    ]
                    for (field, value) in judged { metrics.set(key + field, value) }
                    metrics.set(key + "_read_wrong", r.readWrong)
                    metrics.set(key + "_sample_wrong", r.sampleWrong)
                    metrics.set(key + "_minmag_sample_code", r.minmagCode)
                    checks += judged.map { ("\(key)\($0.0)=\($0.1)", $0.1 == 0) }
                } catch {
                    checks.append(("\(c.format): \(GPU.describe(error))", false))
                }
            }
            return checks
        }
    }
}

/// Views and shared samplers of the packed textures of gpu.gm20b.packed_levels, against an M4 Max under macOS 27.2.1
/// (gpu/PackedLevelsOpen.swift, this scene, three runs): views of levels 1-2, 2 and 0 are made at those levels'
/// sizes and a kernel sees them so (packed_open_views_made_<format> 3, _views_wrong 0); a kernel's one constexpr
/// sampler over such a texture and an RGBA8 8x8 completes and samples the RGBA8 texels exactly
/// (packed_open_shared_off_<format> 0).
struct PackedLevelsOpenTest: HWTest {
    let id = "gpu.gm20b.packed_levels_open"
    let name = "Views of packed 10-bit textures of three levels and samplers shared with them work as Metal's, on raw"
    let subsystem = Subsystem.metal
    var timeout: TimeInterval { 30 }

    func run(_ context: TestContext) async throws -> Outcome {
        try await CompilerShapeRun.judged { gpu, metrics in
            var checks: [(String, Bool)] = []
            for c in PackedLevels.cases {
                do {
                    let v = try await PackedLevelsOpen.views(gpu, c)
                    metrics.set("packed_open_views_made_\(c.format)", v.made)
                    metrics.set("packed_open_views_wrong_\(c.format)", v.wrong)
                    checks.append(("\(c.format): \(v.made) of 3 views made", v.made == 3))
                    checks.append(("\(c.format): \(v.wrong) views off", v.wrong == 0))
                } catch {
                    checks.append(("\(c.format) views: \(GPU.describe(error))", false))
                }
                do {
                    let s = try await PackedLevelsOpen.sharedSampler(gpu, c)
                    metrics.set("packed_open_shared_off_\(c.format)", s.off)
                    checks.append(("\(c.format): shared sampler \(s.path)", s.path == "gm20b"))
                    checks.append(("\(c.format): \(s.off) off", s.off == 0))
                } catch {
                    checks.append(("\(c.format) shared sampler: \(GPU.describe(error))", false))
                }
            }
            return checks
        }
    }
}
