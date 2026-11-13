import Foundation

public enum SM53Reference {
    public static func flushedHalf(_ value: Float16) -> Float {
        if value.bitPattern & 0x7C00 == 0 {
            return Float(bitPattern: UInt32(value.bitPattern & 0x8000) << 16)
        }
        return Float(value)
    }

    public static func halfResult(_ x: Float16, _ y: Float16, _ z: Float16) -> [Float] {
        let sum = flushedHalf(x + y), product = flushedHalf(x * y)
        let fma = flushedHalf(z.addingProduct(x, y))
        return [sum, product, sum, product, fma]
    }

    public static func truncatedSource(_ value: Float) -> Float16 {
        let magnitude = value.bitPattern & 0x7FFF_FFFF
        let sign = UInt16((value.bitPattern >> 16) & 0x8000)
        let bits: UInt16
        if magnitude > 0x7F80_0000 {
            bits = 0x7E00
        } else if magnitude == 0x7F80_0000 {
            bits = 0x7C00
        } else if magnitude >= 0x4780_0000 {
            bits = 0x7BFF
        } else if magnitude < 0x3880_0000 {
            bits = 0
        } else {
            bits = UInt16((magnitude >> 13) - 0x1C000)
        }
        return Float16(bitPattern: sign | bits)
    }

    public static func same(_ a: Float, _ b: Float) -> Bool {
        a.bitPattern == b.bitPattern || (a.isNaN && b.isNaN)
    }
}
