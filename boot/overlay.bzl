"""The overlay's Mach-Os //boot:package trusts, each by the source tree/overlay/manifest.json names it by. The release
carries each at overlay/<source> (//install:release_inputs), which @switch_release names overlay/<source>."""

OVERLAY_BINARIES = {
    "//drivers/SwitchBluetooth:BlueTool": "build/switch-bluetooth/BlueTool",
    "//drivers/SwitchAudio:SwitchAudioHAL": "build/switch-audio/SwitchAudio.driver/SwitchAudio",
    "//drivers/SwitchJoyCon:joyconpaird": "build/switch-joycon/joyconpaird",
    "//drivers/TegraGM20BMetal:SwitchShaderCompiler": "build/switch-gpu/SwitchShaderCompiler",
    "//drivers/SwitchAppRegistration:switch-appregd": "build/switch-apps/switch-appregd",
    "//drivers/SwitchVideoDecoders:HEVCH8": "build/switch-video/HEVCH8.videodecoder",
    "//drivers/SwitchVideoDecoders:AVD": "build/switch-video/AVD.videodecoder",
    "//drivers/SwitchVideoEncoders:AVE": "build/switch-video/H264H7.videoencoder",
}
