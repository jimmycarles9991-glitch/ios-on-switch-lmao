/// An M4 Max's bytes of XrSrgbScene's draws (validation off; the scene's draws run there by a copy of its code): each
/// texel as a little-endian word. drawn: the unblended draw over a clear; blended: the source-over draw over drawn's
/// codes; cleared: each of XrSrgb.clears.
extension XrSrgb {
    static let drawn553: [UInt64] = [
        0x9fc0_bdc0_bdc0_bdc0, 0x6000_0000_fb00_6100, 0x62c0_0000_f980_6200, 0x7100_02c0_f800_6300,
        0x7f40_05c0_f680_6400, 0x8d40_08c0_f500_6540, 0x9b80_0c00_f340_6640, 0xa9c0_0f80_f1c0_6700,
        0xb7c0_1340_f000_67c0, 0xc600_1740_ee40_6000, 0xd440_1b80_ecc0_6100, 0xdf80_2000_eb00_6200,
        0xdf80_2500_e940_6300, 0x6000_2ac0_e780_6400, 0x6000_3140_e580_6540, 0x65c0_3900_e3c0_6640,
        0x73c0_4340_e1c0_6700, 0x8200_56c0_e000_67c0, 0x9040_7780_ddc0_6000, 0x9e40_83c0_dbc0_6100,
        0xac80_8c40_d9c0_6200, 0xbac0_9300_d7c0_6300, 0xc8c0_9900_d580_6400, 0xd700_9e40_d340_6540,
        0xdf80_a300_d100_6640, 0xdf80_a740_cec0_6700, 0x6000_ab40_cc40_67c0, 0x6000_af00_ca00_6000,
        0x6880_b2c0_c740_6100, 0x76c0_b600_c4c0_6200, 0x84c0_b940_c200_6300, 0x9300_bc40_bf40_6400,
        0xa140_bf40_bc40_6540, 0xaf40_c200_b940_6640, 0xbd80_c4c0_b600_6700, 0xcbc0_c740_b2c0_67c0,
        0xd9c0_ca00_af00_6000, 0xdf80_cc40_ab40_6100, 0xdf80_cec0_a740_6200, 0x6000_d100_a300_6300,
        0x6000_d340_9e40_6400, 0x6b40_d580_9900_6540, 0x7980_d7c0_9300_6640, 0x87c0_d9c0_8c40_6700,
        0x95c0_dbc0_83c0_67c0, 0xa400_ddc0_7780_6000, 0xb240_e000_56c0_6100, 0xc040_e1c0_4340_6200,
        0xce80_e3c0_3900_6300, 0xdcc0_e580_3140_6400, 0xdf80_e780_2ac0_6540, 0xdf80_e940_2500_6640,
        0x6000_eb00_2000_6700, 0x6000_ecc0_1b80_67c0, 0x6e40_ee40_1740_6000, 0x7c40_f000_1340_6100,
        0x8a80_f1c0_0f80_6200, 0x98c0_f340_0c00_6300, 0xa6c0_f500_08c0_6400, 0xb500_f680_05c0_6540,
        0xc340_f800_02c0_6640, 0xd140_f980_0000_6700, 0xdf80_fb00_0000_67c0, 0xdf80_fc80_0000_6000,
    ]
    static let blended553: [UInt64] = [
        0x9fc0_bdc0_bdc0_bdc0, 0x7980_0ac0_ee40_a2c0, 0x94c0_2780_e240_bb00, 0xb340_9e00_d980_cc40,
        0xcc40_cb00_d540_da40, 0xdf80_2580_d5c0_e600, 0xa480_12c0_f0c0_9600, 0xbbc0_3280_da00_b180,
        0xcd00_a040_cd40_c3c0, 0xd8c0_c8c0_c4c0_d200, 0xdec0_2800_c1c0_ddc0, 0xdf80_2340_e940_8540,
        0xdf80_4c80_e480_a700, 0x9b80_a500_e3c0_bac0, 0xb500_c800_b200_c980, 0xcf40_2e40_a900_d5c0,
        0x73c0_4340_e1c0_6700, 0x94c0_8900_da00_9c40, 0xb000_ab80_d680_b180, 0xc580_c880_d840_c100,
        0xd540_3ac0_de40_cd40, 0xdf80_9a80_6000_d7c0, 0xcbc0_9f00_cfc0_8fc0, 0xd9c0_b380_c8c0_a840,
        0xdf80_cac0_c740_b840, 0xdf80_6a80_cbc0_c500, 0xd700_9ec0_d500_cf40, 0x6880_b000_cb80_8000,
        0x8840_bc80_ba00_9e00, 0xa780_ce40_b400_af80, 0xc140_8d80_b580_bc40, 0xd540_a540_be00_c6c0,
        0xa140_bf40_bc40_6540, 0xb900_c5c0_bfc0_9400, 0xcb00_d2c0_c940_a680, 0xd780_a0c0_9640_b3c0,
        0xde80_ad80_9e00_be00, 0xdf80_c080_ae00_c6c0, 0xdf80_cf80_aac0_8800, 0x8a80_d880_b300_9d00,
        0xa400_b0c0_c100_aac0, 0xc080_b680_d240_b540, 0xd8c0_c400_6bc0_bd80, 0x8d80_d980_8d40_7bc0,
        0xa980_dec0_9380_93c0, 0xbfc0_bf40_a440_a180, 0xd080_c040_b8c0_ac00, 0xdb40_c880_ce00_b440,
        0xce80_e3c0_3900_6300, 0xdd40_e600_3600_8980, 0xdf80_ccc0_4c40_9880, 0xdf80_ca00_9200_a2c0,
        0xc600_ce80_b080_aa80, 0xdf80_d900_ca80_b0c0, 0x7d40_ed80_2c80_7e40, 0x9d40_d980_9080_8e80,
        0xb7c0_d440_2740_98c0, 0xccc0_d540_7200_a040, 0xdbc0_dc80_a880_a5c0, 0xb7c0_f5c0_0b80_7380,
        0xcac0_e5c0_2480_8500, 0xd7c0_de80_8e40_8ec0, 0xdf80_dcc0_bd00_9540, 0xdf80_e0c0_3a80_99c0,
    ]
    static let cleared553: [UInt64] = [
        0x9fc0_bdc0_bdc0_bdc0, 0xb940_1b80_f3c0_6500, 0xdf80_ff40_0080_9dc0, 0x6000_6040_7c40_d9c0,
    ]
    static let drawn555: [UInt32] = [
        0xef7b_def7, 0xc00f_b184, 0xc00f_9988, 0xc0bf_818c, 0xc17f_6990, 0xc23f_5195, 0xc30f_3599, 0xc3ef_1d9c,
        0xc4df_019f, 0xc5de_e580, 0xc6ee_cd84, 0xc80e_b188, 0xc94e_958c, 0xcabe_7990, 0xcc5e_5995, 0xce4e_3d99,
        0xd0de_1d9c, 0xd5be_019f, 0xdded_dd80, 0xe0fd_bd84, 0xe31d_9d88, 0xe4cd_7d8c, 0xe64d_5990, 0xe79d_3595,
        0xe8cd_1199, 0xe9dc_ed9c, 0xeadc_c59f, 0xebcc_a180, 0xecbc_7584, 0xed8c_4d88, 0xee5c_218c, 0xef1b_f590,
        0xefdb_c595, 0xf08b_9599, 0xf13b_619c, 0xf1db_2d9f, 0xf28a_f180, 0xf31a_b584, 0xf3ba_7588, 0xf44a_318c,
        0xf4d9_e590, 0xf569_9195, 0xf5f9_3199, 0xf678_c59c, 0xf6f8_3d9f, 0xf777_7980, 0xf805_6d84, 0xf874_3588,
        0xf8f3_918c, 0xf963_1590, 0xf9e2_ad95, 0xfa52_5199, 0xfac2_019c, 0xfb31_b99f, 0xfb91_7580, 0xfc01_3584,
        0xfc70_f988, 0xfcd0_c18c, 0xfd40_8d90, 0xfda0_5d95, 0xfe00_2d99, 0xfe60_019c, 0xfec0_019f, 0xff20_0180,
    ]
    static let blended555: [UInt32] = [
        0xef7b_def7, 0xc2be_1a8b, 0xc9ed_8eec, 0xe78d_3b31, 0xf2cd_2b69, 0xc96d_5f98, 0xc4be_b258, 0xccad_5ec6,
        0xe81c_b30f, 0xf23c_4348, 0xca0c_1f77, 0xc8de_9615, 0xd32e_4a9c, 0xe94e_3eeb, 0xf20b_2326, 0xcb9a_9357,
        0xd0de_1d9c, 0xe24d_a271, 0xeaed_6ac6, 0xf22d_8704, 0xcebd_e735, 0xe6a6_035f, 0xe7cc_fe3f, 0xecec_8ea1,
        0xf2bc_76e1, 0xdaac_bf14, 0xe7bd_533d, 0xec0c_ba00, 0xef2b_a278, 0xf39b_42be, 0xe36b_5af1, 0xe95b_e31b,
        0xefdb_c595, 0xf17b_fe50, 0xf4bc_969a, 0xe839_66cf, 0xeb69_e2f8, 0xf02a_e31b, 0xf3ea_ae20, 0xf62b_3274,
        0xec3c_12ab, 0xedad_26d5, 0xf106_bef6, 0xf668_d5ef, 0xf7b9_3a4f, 0xefda_4686, 0xf01b_8eb0, 0xf22c_e2d1,
        0xf8f3_918c, 0xf983_6226, 0xf334_c662, 0xf289_228b, 0xf3ab_0aaa, 0xf64c_aac3, 0xfb12_c9f9, 0xf5b9_0a3a,
        0xf442_7663, 0xf4c7_2281, 0xf6fa_8a97, 0xfb20_b9ce, 0xf6f2_4a14, 0xf578_e63b, 0xf5ab_d255, 0xf783_aa67,
    ]
    static let cleared555: [UInt32] = [
        0xef7b_def7, 0xc6ef_3d94, 0xffd0_0a77, 0xd817_c767,
    ]
}
