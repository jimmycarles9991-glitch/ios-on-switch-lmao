/// gpu.gm20b.xr_blend_routes's golden scene on an M4 Max under macOS 27.2.1, its shaders compiled as the app's metallib is
/// (rules_apple's metal compile passes -ffast-math), the same in four runs: each region's texel, row band by row band
/// (550: the alpha byte above the code word; 552: the four 16-bit channels, blue lowest). Compiled with -fno-fast-math,
/// the same GPU stores one code more in 550's regions 8 (red) and 11 (blue) and 552's regions 6 (alpha), 8 (red) and 11
/// (blue).
enum XrBlendRoutesGoldens {
    static let xr550: [UInt64] = [
        0x8c_1dca_9524, 0xa3_225a_3955, 0xbf_27d9_cdbe, 0xd8_2d09_65a5, 0xec_30d9_19df, 0xf7_3328_ea5e, 0xfd_3438_d5c2,
        0xff_34a8_ce08, 0xe5_3ee2_fa9e, 0xf2_3ff5_488f, 0xf6_3f8f_5800, 0xfb_0fdf_fee4, 0xff_3f1f_ffff, 0xff_2e50_6de6,
        0xff_2939_47f8, 0xff_0a18_97ff, 0xbf_2659_fd80, 0xd9_08aa_c9e6, 0xe0_121b_6e38, 0xe0_154f_6ad1, 0xf3_207a_9339,
        0xf7_2453_3135, 0xfd_2ba8_5a02, 0xfe_3ff7_f841, 0xff_1e69_32b2, 0xff_2e5d_2fb1, 0xff_3ffb_0ba5, 0xff_002f_fa7f,
        0xff_1ab7_01b8, 0xff_2dd2_3aea, 0xff_22f4_1e35, 0xff_3ff0_03ab,
    ]
    static let xr552: [UInt64] = [
        0xa600_7700_a940_4900, 0xb180_8940_a380_5540, 0xbf80_9f40_9cc0_6f80, 0xcc40_b400_9640_6940,
        0xd5c0_c340_9180_77c0, 0xdbc0_cc80_8e80_9780, 0xde40_d0c0_8d40_7080, 0xdf40_d280_8cc0_8200,
        0xd2c0_fb80_2f80_a780, 0xd940_ffc0_5480_23c0, 0xdac0_fe00_f580_0000, 0xddc0_3f40_ffc0_b900,
        0xdf40_fc40_ffc0_ffc0, 0xdf80_b940_06c0_7980, 0xdf80_a4c0_9440_fe00, 0xdf80_2840_8940_ffc0,
        0xbf80_9940_9fc0_6000, 0xcc80_2280_ac80_7980, 0xd040_4840_b6c0_8e00, 0xd040_5500_f680_b440,
        0xd980_81c0_a900_ce40, 0xdb40_9140_3300_4d40, 0xde40_ae80_8580_8080, 0xdf00_ffc0_7f80_1040,
        0xdf80_7980_9300_ac80, 0xdf80_b940_d2c0_ec40, 0xdf80_ffc0_b080_e940, 0xdf80_0080_ff80_9fc0,
        0xdf80_6ac0_7000_6e00, 0xdf80_b740_2380_ba80, 0xdf80_8bc0_41c0_8d40, 0xdf80_ffc0_0000_eac0,
    ]
}
