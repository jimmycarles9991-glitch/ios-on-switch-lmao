import Darwin
import Foundation
import notify

/// The raw path's DEVELOPMENT switches the glass tests post to every process (raw/runtime/RawMetalGlassProbe.mm), and
/// the render server's record of the command buffers it commits while the glass probe is armed, read from notify state.
/// A release build's processes register none of them: the record then stays empty and the switches change nothing.
enum GlassProbe {
    static let prefix = "com.brunostjohn.switch.debug."
    static let recordName = prefix + "glass-cb"
    static let recordSlots: UInt64 = 16

    /// One command buffer of the record (raw_glass_probe.h GlassCbEntry): API draws and encoders, passes into a mip
    /// level above 0 (the backdrop pyramid Core Animation regenerates), and the commit's mach time's low 40 bits.
    struct Buffer: Equatable {
        let draws: Int
        let encoders: Int
        let levels: Int
        let ticks: UInt64

        init(word: UInt64) {
            draws = Int(word & 1023)
            encoders = Int(word >> 10 & 255)
            levels = Int(word >> 18 & 63)
            ticks = word >> 24
        }

        var text: String { "\(draws)/\(encoders)" + (levels > 0 ? "L\(levels)" : "") }
    }

    static func post(_ name: String) {
        notify_post(prefix + name)
    }

    /// The draw lines and the record, for 20 s from now in every process: a new generation, so each snapshot's lines
    /// and record start empty. Returns once the render server took it (its record's generation moved), or after 200 ms.
    /// `lines` false arms the record alone.
    @discardableResult
    static func arm(lines: Bool = true) -> Bool {
        let before = count()?.generation
        post(lines ? "glass-probe" : "glass-record")
        for _ in 0..<20 {
            usleep(10_000)
            if let now = count()?.generation, now != before { return true }
        }
        return false
    }

    static func disarm() { post("glass-probe-off") }

    static func lowTicks() -> UInt64 { mach_absolute_time() & ((1 << 40) - 1) }

    static func state(_ name: String) -> UInt64? {
        var token: Int32 = 0
        guard notify_register_check(name, &token) == NOTIFY_STATUS_OK else { return nil }
        defer { notify_cancel(token) }
        var state: UInt64 = 0
        return notify_get_state(token, &state) == NOTIFY_STATUS_OK ? state : nil
    }

    /// The record's count and generation, nil before the render server armed it once.
    static func count() -> (generation: UInt64, count: UInt64)? {
        guard let state = state(recordName), state != 0 else { return nil }
        return (state >> 32, state & 0xffff_ffff)
    }

    /// The command buffers recorded after the `after`th, oldest first, as many as the record still holds; none when an
    /// arming started the record again meanwhile.
    static func buffers(after: UInt64) -> (count: UInt64, buffers: [Buffer], lost: UInt64) {
        guard let (_, count) = count() else { return (after, [], 0) }
        guard count >= after else { return (count, [], 0) }
        let first = max(after, count > recordSlots ? count - recordSlots : 0)
        let buffers = (first..<count).compactMap { index in
            state("\(recordName).\(index % recordSlots)").map(Buffer.init(word:))
        }
        return (count, buffers, first - min(after, first))
    }

    /// A snapshot's encoding, from the command buffers the render server committed while it ran: A renders passes into
    /// a mip level (it regenerates the backdrop pyramid), B renders none (it reuses the last one), "-" no record.
    static func encoding(_ buffers: [Buffer]) -> String {
        if buffers.isEmpty { return "-" }
        return buffers.contains { $0.levels > 0 } ? "A" : "B"
    }

    static func signature(_ buffers: [Buffer]) -> String {
        buffers.isEmpty ? "-" : buffers.map(\.text).joined(separator: ",")
    }
}

/// The command buffers the render server commits while `body` runs, with the record armed for it (and the draw lines
/// with `lines`).
struct GlassRecording {
    let buffers: [GlassProbe.Buffer]
    let lost: UInt64
    /// The glass probe's generation the render server took: the `g<generation>` of its draw lines and record lines.
    let generation: UInt64

    var encoding: String { GlassProbe.encoding(buffers) }
    var signature: String { GlassProbe.signature(buffers) }

    @MainActor
    static func around<R>(lines: Bool = false, _ body: () -> R) -> (R, GlassRecording) {
        GlassProbe.arm(lines: lines)
        let (generation, start) = GlassProbe.count() ?? (0, 0)
        let result = body()
        usleep(30_000)
        let read = GlassProbe.buffers(after: start)
        return (result, GlassRecording(buffers: read.buffers, lost: read.lost, generation: generation))
    }
}
