import Foundation

/// One of gpu.gm20b.compressed_targets' ZBC arms judged: its first clear's texels off the value, its last clear's off
/// the first's, its full clears that hit an entry, and its entry asks the kernel refused. A refused ask leaves the
/// arm's clears ordinary ones, so ZBC was never exercised for its format: that fails on its own, never as a pass.
public enum ZbcArmVerdict {
    public static func wrong(_ arm: String, firstOff: Int, lastOff: Int, hits: Int, refused: Int) -> [String] {
        var wrong: [String] = []
        if firstOff > 0 { wrong.append("zbc_\(arm)_first") }
        if lastOff > 0 { wrong.append("zbc_\(arm)_held_texel") }
        if refused > 0 {
            wrong.append("zbc_\(arm)_table_full")
        } else if hits == 0 {
            wrong.append("zbc_\(arm)_no_hit")
        }
        return wrong
    }
}
