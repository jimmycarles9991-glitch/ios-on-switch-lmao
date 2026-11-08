import Foundation
import HWTestCore
import SwitchVPX

/// The VP9 clips, from libvpx: VideoToolbox has no VP9 encoder. One thread and no lag (so no alt-ref frames, one
/// frame per packet) keep the bitstream deterministic and directly decodable frame by frame.
enum VP9Writer {
    struct Failure: Error, CustomStringConvertible {
        let description: String
    }

    static func write(_ clip: Clip, to url: URL) throws {
        var config = vpx_codec_enc_cfg_t()
        try check(vpx_codec_enc_config_default(vpx_codec_vp9_cx(), &config, 0), "config")
        config.g_w = UInt32(clip.width)
        config.g_h = UInt32(clip.height)
        config.g_timebase = vpx_rational(num: 1, den: Int32(clip.fps))
        config.rc_target_bitrate = UInt32(clip.bitrate / 1000)
        config.rc_end_usage = VPX_VBR
        config.g_threads = 1
        config.g_lag_in_frames = 0
        config.kf_min_dist = 0
        config.kf_max_dist = UInt32(clip.frames)
        var codec = vpx_codec_ctx_t()
        try check(switch_vp9_init(&codec, &config), "init")
        defer { vpx_codec_destroy(&codec) }
        try check(switch_vp9_set_cpu_used(&codec, 4), "cpu-used")
        guard let image = vpx_img_alloc(nil, VPX_IMG_FMT_I420, UInt32(clip.width), UInt32(clip.height), 16) else {
            throw Failure(description: "vpx_img_alloc failed")
        }
        defer { vpx_img_free(image) }
        let pattern = clip.pattern
        var frames: [IVF.Frame] = []
        for index in 0..<clip.frames {
            let planes = image.pointee.planes
            let strides = image.pointee.stride
            PatternWriter.writeLuma(pattern, frame: index, into: .init(base: planes.0!, rowBytes: Int(strides.0)))
            PatternWriter.writeChromaPlanar(
                pattern, cb: .init(base: planes.1!, rowBytes: Int(strides.1)),
                cr: .init(base: planes.2!, rowBytes: Int(strides.2)))
            let flags = index == 0 ? vpx_enc_frame_flags_t(VPX_EFLAG_FORCE_KF) : 0
            try check(
                vpx_codec_encode(&codec, image, vpx_codec_pts_t(index), 1, flags, UInt(VPX_DL_GOOD_QUALITY)), "frame")
            drain(&codec, into: &frames)
        }
        try check(vpx_codec_encode(&codec, nil, -1, 1, 0, UInt(VPX_DL_GOOD_QUALITY)), "flush")
        drain(&codec, into: &frames)
        guard frames.count == clip.frames else {
            throw Failure(description: "\(clip.file): libvpx returned \(frames.count) of \(clip.frames) frames")
        }
        let header = IVF.Header(
            fourcc: "VP90", width: clip.width, height: clip.height, rate: clip.fps, scale: 1, frameCount: frames.count)
        try IVF.write(header, frames: frames).write(to: url)
    }

    static func drain(_ codec: inout vpx_codec_ctx_t, into frames: inout [IVF.Frame]) {
        var iterator: vpx_codec_iter_t?
        while let packet = vpx_codec_get_cx_data(&codec, &iterator) {
            var size = 0
            var pts: vpx_codec_pts_t = 0
            if let bytes = switch_vpx_frame(packet, &size, &pts) {
                frames.append(IVF.Frame(pts: UInt64(pts), data: Data(bytes: bytes, count: size)))
            }
        }
    }

    static func check(_ status: vpx_codec_err_t, _ step: String) throws {
        guard status == VPX_CODEC_OK else {
            throw Failure(description: "libvpx \(step): \(String(cString: vpx_codec_err_to_string(status)))")
        }
    }
}
