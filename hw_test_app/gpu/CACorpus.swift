import Foundation
import HWTestCore

/// Core Animation states the GPU scenes make, verbatim from drivers/TegraGM20BMetal/precompile/corpus.log (the raw
/// pack's corpus): PBGRAXm_A2Xhf, the untextured fill (fixed_frag with no image) into BGRA8 beside the memoryless
/// colour 1, the backdrop blur's chain (downsample, narrow blurs, luma boost) into 'b3a8' (550) and BGR10_XR (554),
/// and the variable blur's downsample into RGBA16Float (115) and BGRA8 (80).
public enum CACorpus {
    static let lines = """
        R ok=1 label=com.apple.coreanimation.PBGRAXm_A2Xhf samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v fixed_vert_lph_spc fc_generic:53:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_vertex_layout:49:00 | f fixed_frag_lph_cph fc_attachment_count:49:02 fc_blend_function:49:3a fc_clamp_color:53:00 fc_color_inputs0:49:00 fc_color_inputs1:49:00 fc_color_opaque0:53:00 fc_color_opaque1:53:00 fc_color_opcodes0:52:00000000 fc_color_opcodes1:52:00000000 fc_coord_function0:49:00 fc_coord_function1:49:00 fc_debug_xr:53:00 fc_dest_count:49:00 fc_dest_index:49:00 fc_destination_function:49:00 fc_extended_range:53:00 fc_filter0:49:00 fc_filter1:49:00 fc_framebuffer_fetch:53:01 fc_generic:53:00 fc_has_asg_77_weights:53:00 fc_has_attachment_0:53:00 fc_has_attachment_1:53:00 fc_has_attachment_2:53:00 fc_has_color_cube0:53:00 fc_has_color_cube1:53:00 fc_has_color_params:53:00 fc_has_color_trc0:53:00 fc_has_color_trc1:53:00 fc_has_edr_factor:53:00 fc_has_gamma_luts:53:00 fc_has_hdr_scale:53:00 fc_has_image_0A:53:00 fc_has_image_0B:53:00 fc_has_image_0C:53:00 fc_has_image_1A:53:00 fc_has_image_1B:53:00 fc_has_image_1C:53:00 fc_has_lod_bias:53:00 fc_has_lut_tex:53:00 fc_has_noise_scale:53:00 fc_has_noise_tex:53:00 fc_has_premultiply:53:00 fc_has_ycbcr_matrix:53:00 fc_image_count:49:00 fc_image_function0:49:00 fc_image_function1:49:00 fc_memoryless_index0:49:00 fc_memoryless_index1:49:00 fc_noise_width:49:00 fc_plane_count0:49:00 fc_plane_count1:49:00 fc_texcoord_count0:49:00 fc_texcoord_count1:49:00 fc_texture_function:49:00 fc_unbounded_range:53:00 fc_vertex_layout:49:00 |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tds4A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f downsample_4_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tds8A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f downsample_8_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tn11A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f narrow_blur_11_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tn15A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f narrow_blur_15_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tn19A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f narrow_blur_19_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tn23A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f narrow_blur_23_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_Tn27A2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f narrow_blur_27_frag_lph  |
        R ok=1 label=com.apple.coreanimation.Pb3a8Xm_TlubA2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=550:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v std_vert1_lph  | f luma_boost_lph  |
        R ok=1 label=com.apple.coreanimation.Pw30rXm_TlubA2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=554:15:0:1:0:0:1:0:0 c1=550:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v std_vert1_lph  | f luma_boost_lph  |
        R ok=1 label=com.apple.coreanimation.PRGhAXm_TvrdA2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=115:15:0:1:0:0:1:0:0 c1=115:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f variable_blur_downsample_frag_lph  |
        R ok=1 label=com.apple.coreanimation.PBGRAXm_TvrdA2Xhf_Isrc samples=1 topo=0 ds=0/0 c0=80:15:0:1:0:0:1:0:0 c1=80:0:0:1:0:0:1:0:0 a0=31:0:1:48:1:1 a1=29:16:1:48:1:1 a2=29:24:1:48:1:1 a3=27:32:1:48:1:1 | v downsample_blur_vert_lph  | f variable_blur_downsample_frag_lph  |
        """

    public static func state(_ shortLabel: String) -> PipelineLine? {
        lines.split(separator: "\n").compactMap { PipelineLine(String($0)) }.first { $0.shortLabel == shortLabel }
    }
}
