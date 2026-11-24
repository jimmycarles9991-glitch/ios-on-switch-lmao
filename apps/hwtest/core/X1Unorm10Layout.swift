/// Odd-height fragment quad padding and coordinate clamping for gpu.x1.rop_unorm10_rounding.
public enum X1Unorm10Layout {
    public static func paddedRows(_ rows: Int) -> Int {
        (rows + 1) & ~1
    }

    public static func paddedElementCount(width: Int, height: Int) -> Int {
        width * paddedRows(height)
    }

    public static func clampedIndex(x: Int, y: Int, width: Int, height: Int) -> Int {
        let cx = min(max(x, 0), width - 1)
        let cy = min(max(y, 0), height - 1)
        return cy * width + cx
    }
}
