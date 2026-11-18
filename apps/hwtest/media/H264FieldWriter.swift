import CoreMedia
import Foundation
import HWTestCore
import HWTestVideo

/// The PAFF clip: field pictures and frame pictures in one interlaced H.264 stream, which nothing on the Mac encodes
/// (x264 codes interlace as MBAFF frames, VideoToolbox's H.264 encoders only tag a field count). Main profile, CAVLC,
/// frame_mbs_only_flag 0 and deblocking off, so each picture reconstructs exactly what is written here:
///   - a group of six frames starts with an IDR field pair (a top field of I_PCM macroblocks and a bottom field
///     predicted from it) or, every other group, an I_PCM frame picture;
///   - every other frame is a top field predicted from the previous frame's top field with one whole-sample vector
///     (the pattern's gradient moves width / 64 samples a frame; its chroma ramp moves with it, 1.5 levels a frame),
///     I_PCM where that does not hold (the barcode's rows, the last column), then a bottom field predicted from that
///     top field (ref_idx 1: a second field's list starts with the previous frame's bottom field, 8.2.4.2.5), which
///     the pattern's rows a field apart match within a level.
/// Both fields of a frame go in the frame's sample.
enum H264FieldWriter {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static let group = 6
    static let log2MaxFrameNum = 4

    static func write(_ clip: Clip, to url: URL) throws {
        let stream = Stream(clip)
        let sps = stream.nal(0x67, stream.sps())
        let pps = stream.nal(0x68, stream.pps())
        var format: CMFormatDescription?
        let status = sps.withUnsafeBufferPointer { spsBytes in
            pps.withUnsafeBufferPointer { ppsBytes in
                var pointers = [spsBytes.baseAddress!, ppsBytes.baseAddress!]
                var sizes = [spsBytes.count, ppsBytes.count]
                return CMVideoFormatDescriptionCreateFromH264ParameterSets(
                    allocator: nil, parameterSetCount: 2, parameterSetPointers: &pointers,
                    parameterSetSizes: &sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &format)
            }
        }
        guard status == noErr, let format else {
            throw Failure(description: "\(clip.file): CMVideoFormatDescriptionCreateFromH264ParameterSets \(status)")
        }
        let frameTime = CMTime(value: 1, timescale: CMTimeScale(clip.fps))
        let samples = try (0..<clip.frames).map { frame in
            var data = Data()
            for picture in stream.frame(frame) {
                let size = UInt32(picture.count)
                data.append(contentsOf: (0..<4).map { UInt8(truncatingIfNeeded: size >> (24 - 8 * $0)) })
                data.append(contentsOf: picture)
            }
            return try Demux.sample(
                data, format: format, pts: CMTimeMultiply(frameTime, multiplier: Int32(frame)), duration: frameTime,
                sync: frame % (2 * group) == 0)
        }
        try MP4Writer.write(samples, format: format, to: url)
    }

    struct Stream {
        let pattern: Pattern
        let widthMbs: Int
        let pairRows: Int
        let shift: Int

        init(_ clip: Clip) {
            pattern = clip.pattern
            widthMbs = clip.width / 16
            pairRows = clip.height / 32
            shift = clip.width / 64
        }

        func sps() -> [UInt8] {
            var w = BitWriter()
            w.put(77, 8)  // Main
            w.put(0, 8)
            w.put(31, 8)
            w.ue(0)
            w.ue(UInt32(log2MaxFrameNum - 4))
            w.ue(0)  // pic_order_cnt_type
            w.ue(4)  // log2_max_pic_order_cnt_lsb_minus4
            w.ue(2)  // max_num_ref_frames
            w.put(0, 1)
            w.ue(UInt32(widthMbs - 1))
            w.ue(UInt32(pairRows - 1))
            w.put(0, 1)  // frame_mbs_only_flag
            w.put(0, 1)  // mb_adaptive_frame_field_flag
            w.put(1, 1)  // direct_8x8_inference_flag
            w.put(0, 2)  // frame_cropping_flag, vui_parameters_present_flag
            w.trailing()
            return w.bytes
        }

        func pps() -> [UInt8] {
            var w = BitWriter()
            w.ue(0)
            w.ue(0)
            w.put(0, 1)  // CAVLC
            w.put(1, 1)  // bottom_field_pic_order_in_frame_present_flag
            w.ue(0)
            w.ue(0)
            w.ue(0)
            w.put(0, 3)
            w.se(0)
            w.se(0)
            w.se(0)
            w.put(1, 1)  // deblocking_filter_control_present_flag
            w.put(0, 2)
            w.trailing()
            return w.bytes
        }

        /// The frame's pictures in decoding order, each a NAL unit.
        func frame(_ frame: Int) -> [[UInt8]] {
            let inGroup = frame % group
            let idrPeriod = frame % (2 * group)
            let frameNum = UInt32(idrPeriod % (1 << log2MaxFrameNum))
            let poc = UInt32(4 * idrPeriod)
            if inGroup == 0 && (frame / group) % 2 == 1 {
                return [nal(0x61, framePicture(frame, frameNum: frameNum, poc: poc))]
            }
            let idr = idrPeriod == 0
            let top = idr ? intraField(frame, poc: poc) : predictedTop(frame, frameNum: frameNum, poc: poc)
            let bottom = predictedBottom(frameNum: frameNum, poc: poc + 1, firstOfIDR: idr)
            return [nal(idr ? 0x65 : 0x61, top), nal(0x61, bottom)]
        }

        func header(
            _ w: inout BitWriter, intra: Bool, parity: Int?, frameNum: UInt32, poc: UInt32, idr: Bool = false,
            refs: UInt32 = 1
        ) {
            w.ue(0)
            w.ue(intra ? 2 : 0)
            w.ue(0)
            w.put(frameNum, log2MaxFrameNum)
            w.put(parity == nil ? 0 : 1, 1)
            if let parity { w.put(UInt32(parity), 1) }
            if idr { w.ue(0) }
            w.put(poc & 0xff, 8)
            if parity == nil { w.se(1) }  // delta_pic_order_cnt_bottom: the bottom field a count later
            if !intra {
                w.put(refs > 1 ? 1 : 0, 1)  // num_ref_idx_active_override_flag
                if refs > 1 { w.ue(refs - 1) }
                w.put(0, 1)  // ref_pic_list_modification_flag_l0
            }
            w.put(0, idr ? 2 : 1)  // dec_ref_pic_marking: sliding window
            w.se(0)  // slice_qp_delta
            w.ue(1)  // disable_deblocking_filter_idc
        }

        func framePicture(_ frame: Int, frameNum: UInt32, poc: UInt32) -> [UInt8] {
            var w = BitWriter()
            header(&w, intra: true, parity: nil, frameNum: frameNum, poc: poc)
            for mb in 0..<(widthMbs * pairRows * 2) {
                w.ue(25)
                pcm(&w, frame: frame, x: mb % widthMbs, y: mb / widthMbs, parity: nil)
            }
            w.trailing()
            return w.bytes
        }

        func intraField(_ frame: Int, poc: UInt32) -> [UInt8] {
            var w = BitWriter()
            header(&w, intra: true, parity: 0, frameNum: 0, poc: poc, idr: true)
            for mb in 0..<(widthMbs * pairRows) {
                w.ue(25)
                pcm(&w, frame: frame, x: mb % widthMbs, y: mb / widthMbs, parity: 0)
            }
            w.trailing()
            return w.bytes
        }

        func predictedTop(_ frame: Int, frameNum: UInt32, poc: UInt32) -> [UInt8] {
            var w = BitWriter()
            header(&w, intra: false, parity: 0, frameNum: frameNum, poc: poc)
            let barcodeRows = (pattern.barcodeHeight / 2 + 15) / 16
            var vectors = [[Int?]](repeating: [Int?](repeating: nil, count: widthMbs), count: pairRows)
            for y in 0..<pairRows {
                for x in 0..<widthMbs {
                    w.ue(0)  // mb_skip_run
                    if y < barcodeRows || x == widthMbs - 1 {
                        w.ue(30)  // I_PCM in a P slice
                        pcm(&w, frame: frame, x: x, y: y, parity: 0)
                        continue
                    }
                    let vector = 4 * shift
                    w.ue(0)  // P_L0_16x16
                    w.se(vector - predictor(vectors, x: x, y: y))
                    w.se(0)
                    w.ue(0)  // coded_block_pattern 0
                    vectors[y][x] = vector
                }
            }
            w.trailing()
            return w.bytes
        }

        func predictedBottom(frameNum: UInt32, poc: UInt32, firstOfIDR: Bool) -> [UInt8] {
            var w = BitWriter()
            header(&w, intra: false, parity: 1, frameNum: frameNum, poc: poc, refs: firstOfIDR ? 1 : 2)
            for _ in 0..<(widthMbs * pairRows) {
                w.ue(0)  // mb_skip_run
                w.ue(0)  // P_L0_16x16
                if !firstOfIDR { w.put(0, 1) }  // ref_idx_l0 1, te(v) with cMax 1
                w.se(0)
                w.se(0)
                w.ue(0)
            }
            w.trailing()
            return w.bytes
        }

        /// 8.4.1.3's predictor for a 16x16 partition with ref_idx_l0 0, horizontal component (every vector here is
        /// horizontal). `vectors`: the field's macroblocks so far, nil for I_PCM.
        func predictor(_ vectors: [[Int?]], x: Int, y: Int) -> Int {
            func neighbour(_ nx: Int, _ ny: Int) -> (available: Bool, ref: Int, vector: Int) {
                guard nx >= 0, ny >= 0, nx < widthMbs, ny < y || (ny == y && nx < x) else { return (false, -1, 0) }
                guard let vector = vectors[ny][nx] else { return (true, -1, 0) }
                return (true, 0, vector)
            }
            let a = neighbour(x - 1, y)
            var b = neighbour(x, y - 1)
            var c = neighbour(x + 1, y - 1)
            if !c.available { c = neighbour(x - 1, y - 1) }
            if !b.available && !c.available && a.available {
                b = a
                c = a
            }
            let matching = [a, b, c].filter { $0.ref == 0 }
            if matching.count == 1 { return matching[0].vector }
            return [a.vector, b.vector, c.vector].sorted()[1]
        }

        /// One macroblock's samples: a field macroblock's rows are its parity's, a frame macroblock's every row.
        func pcm(_ w: inout BitWriter, frame: Int, x: Int, y: Int, parity: Int?) {
            w.align()
            func row(_ r: Int, _ height: Int) -> Int { parity.map { 2 * (height * y + r) + $0 } ?? height * y + r }
            for r in 0..<16 {
                for c in 0..<16 { w.byte(UInt8(pattern.luma(x: 16 * x + c, y: row(r, 16), frame: frame))) }
            }
            for plane in 0..<2 {
                for r in 0..<8 {
                    for c in 0..<8 {
                        let value = pattern.chroma(x: 2 * (8 * x + c), y: 2 * row(r, 8))
                        w.byte(UInt8(plane == 0 ? value.cb : value.cr))
                    }
                }
            }
        }

        /// 7.4.1: an emulation_prevention_three_byte wherever the payload would form a start code.
        func nal(_ header: UInt8, _ rbsp: [UInt8]) -> [UInt8] {
            var out = [header]
            var zeros = 0
            for byte in rbsp {
                if zeros >= 2 && byte <= 3 {
                    out.append(3)
                    zeros = 0
                }
                out.append(byte)
                zeros = byte == 0 ? zeros + 1 : 0
            }
            return out
        }
    }

    struct BitWriter {
        var bytes: [UInt8] = []
        var bits = 0

        mutating func put(_ value: UInt32, _ count: Int) {
            for shift in stride(from: count - 1, through: 0, by: -1) {
                if bits % 8 == 0 { bytes.append(0) }
                if value >> UInt32(shift) & 1 == 1 { bytes[bytes.count - 1] |= UInt8(0x80 >> (bits % 8)) }
                bits += 1
            }
        }

        mutating func ue(_ value: UInt32) {
            let length = 32 - (value + 1).leadingZeroBitCount
            put(0, length - 1)
            put(value + 1, length)
        }

        mutating func se(_ value: Int) { ue(UInt32(value > 0 ? 2 * value - 1 : -2 * value)) }

        mutating func align() {
            while bits % 8 != 0 { put(0, 1) }
        }

        mutating func byte(_ value: UInt8) {
            bytes.append(value)
            bits += 8
        }

        mutating func trailing() {
            put(1, 1)
            align()
        }
    }
}
