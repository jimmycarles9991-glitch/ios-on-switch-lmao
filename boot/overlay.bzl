"""The overlay's Mach-Os //boot:package trusts, each by the source tree/overlay/manifest.json names it by. The release
carries each at overlay/<source> (//install:release_inputs), which @switch_release names overlay/<source>."""

OVERLAY_BINARIES = {
    "//apps/installer:image_ipa": "build/switch-apps/SwitchInstaller.ipa",
    "//drivers/SwitchBluetooth:BlueTool": "build/switch-bluetooth/BlueTool",
    "//drivers/SwitchAudio:SwitchAudioHAL": "build/switch-audio/SwitchAudio.driver/SwitchAudio",
    "//drivers/SwitchJoyCon:joyconpaird": "build/switch-joycon/joyconpaird",
    "//drivers/TegraGM20BMetal:SwitchShaderCompiler": "build/switch-gpu/SwitchShaderCompiler",
    "//drivers/SwitchAppRegistration:switch-appregd": "build/switch-apps/switch-appregd",
    "//drivers/SwitchInstallPolicy:SwitchInstallPolicy": "build/switch-apps/libSwitchInstallPolicy.dylib",
    "//drivers/SwitchBootstrap:switch-bootstrap": "build/switch-bootstrap/switch-bootstrap",
    "//drivers/NativeSwitchBlock:switch-sdwake": "build/switch-block/switch-sdwake",
    "//drivers/SwitchVideoDecoders:HEVCH8": "build/switch-video/HEVCH8.videodecoder",
    "//drivers/SwitchVideoDecoders:AVD": "build/switch-video/AVD.videodecoder",
    "//drivers/SwitchVideoEncoders:AVE": "build/switch-video/H264H7.videoencoder",
}

BOOTSTRAP_PACKAGES = {
    package: "build/switch-bootstrap/packages/" + package + ".deb"
    for package in ["sileo", "ellekit", "uikittools", "launchctl"]
}

BOOTSTRAP_DEVELOPMENT_PACKAGES = {
    "//bootstrap:development_" + package.replace("-", "_"): "build/switch-bootstrap/packages/" + package + ".deb"
    for package in ["openssh-client", "openssh-sftp-server", "openssh-server", "switch-ssh", "top"]
}
