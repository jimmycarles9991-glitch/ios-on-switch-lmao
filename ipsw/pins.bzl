"""The IPSW kinds the build knows, by build: the one table that names an IPSW, its build and its member files.

Written by `bazel run //ipsw:pin -- --kind=<build> <ipsw url> <product> <device class>` (ipsw/pin.py),
which pins one kind's members and keeps the rest. Per kind:
  members   each member the build or the SD install reads: where it is stored in the IPSW (offset, stored_size), the
            sha256 of its stored bytes (what Bazel fetches by range request) and of its content
  ios, xnu  pinned by hand: the iOS major (//ipsw:ios<major>, --config=ios<major>) and the XNU the kernel is built
            from for it (kernel/xnu-<xnu>, //kernel:xnu_<xnu>)
  volumes   pinned by hand: the APFS volume UUIDs of the OS image and the two cryptexes (their volume superblocks,
            read from the decrypted images) and the cryptex images' file names as Preboot stages them
  derived   what the build derives from the members, pinned by hand: kernelcache_sha256 (the kernelcache's IM4P payload,
            LZFSE-decompressed, its arm64 slice: //ipsw:kernelcache), quartzcore_metallib_sha256 (QuartzCore's
            default.metallib, the OS volume's or the SystemOS cryptex's grafted over it: the shader seed's gate,
            //gpu/seed:seed) and, for an arm64e IPSW, pretranslated: {file: sha256} of ipsw/pretranslated/<kind>/, the
            trust modules and dyld cache list of //translate/arm64e/pretranslate's run over the decrypted images
            (//ipsw:pretranslated), and kernel_virt_base, the prelinked kernel's lowest segment (its kexts sit below
            XNU's own __TEXT): the loader's layout refuses an image that differs (tools/package/build_native_loader.py)
            and the kernel derives the virtBase it expects from it (kernel/switch/include/switch/boot_layout.h).
            21H461's was measured on the device
An Apple Encrypted Archive member (iOS 26 and later) also has aea_key, the key /usr/bin/aea decrypts it with, which
`pin` unwraps from the archive's auth data. An empty kind is a declared slot: //ipsw:kind accepts it, no repository is
made for it, and anything that reads an IPSW fails at analysis until it is pinned.
"""

# --//ipsw:kind's default.
DEFAULT_KIND = "21H461"

IPSWS = {
    "21H461": {
        "build": "21H461",
        "derived": {
            "kernel_virt_base": "0xfffffff00427c000",
            "kernelcache_sha256": "f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7",
            "quartzcore_metallib_sha256": "606f913adf520420f9b4bb666efb782a68c830f082f1e559e17b20d0d13adbc9",
        },
        "device_class": "j120ap",
        "ios": "17",
        "members": {
            "app_cryptex": {
                "component": "Cryptex1,AppOS",
                "content_sha256": "52e568e1eaaede5fde4684ad10afc60f8fb236bfb5a23e0869bb0ed8a1d275d7",
                "deflated": True,
                "member": "044-75188-038.dmg",
                "offset": 67,
                "sha256": "c3ef4829e3b795915aec53eeeb470ee817f97b8c27f4eaceeb6c4f18119fd6ff",
                "size": 10485760,
                "stored_size": 2781428,
            },
            "app_cryptex_trust_cache": {
                "component": "Cryptex1,AppTrustCache",
                "content_sha256": "5d9bf23c900cb0b6ea241b1208a08aeef9cd863536452d85e68d1c0ef98e77c4",
                "deflated": True,
                "member": "Firmware/044-75188-038.dmg.trustcache",
                "offset": 6749437699,
                "sha256": "6244096f060c47c03b677bc1cb7e3f061bbd58fafc9a6ac144fb0544ee7f52a3",
                "size": 767,
                "stored_size": 739,
            },
            "device_tree": {
                "component": "DeviceTree",
                "content_sha256": "3f612f64819fbf6a8aacc03fabeda08ba897c4bf5385df995c59815c0c80c8f0",
                "deflated": True,
                "member": "Firmware/all_flash/DeviceTree.j120ap.im4p",
                "offset": 6773560004,
                "sha256": "b6a41976d2fe70feb4d4d1a3b499df6502d9e6acbf6a91c88b27973355d096ec",
                "size": 35260,
                "stored_size": 35169,
            },
            "kernelcache": {
                "component": "KernelCache",
                "content_sha256": "13b97fa2c5ce97f04408dcc28c41fb6b0d60b9440065197879acf44b2572bf7b",
                "deflated": True,
                "member": "kernelcache.release.ipad7",
                "offset": 6818903409,
                "sha256": "8c1a97e43fab66baa0be3584e933895f90cfd12ee04e2cbc0e64c3b04feb5bf3",
                "size": 16238580,
                "stored_size": 16214447,
            },
            "os": {
                "component": "OS",
                "content_sha256": "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d",
                "deflated": False,
                "member": "044-76040-038.dmg",
                "offset": 1591988020,
                "sha256": "3f99b859c9728379110b66657c7b15285bcb1925ad8b5726542e2cdcf8b1028d",
                "size": 5157406069,
                "stored_size": 5157406069,
            },
            "static_trust_cache": {
                "component": "StaticTrustCache",
                "content_sha256": "4563bdbf6ed8f72237d45dbba3ccde432634338f95c10d79d5d4631678a97c7c",
                "deflated": True,
                "member": "Firmware/044-76040-038.dmg.trustcache",
                "offset": 6773229318,
                "sha256": "91433644b887568d5c052607aa7dd2a16eb85745378f480d5b94b7df0993bd44",
                "size": 59519,
                "stored_size": 54001,
            },
            "system_cryptex": {
                "component": "Cryptex1,SystemOS",
                "content_sha256": "b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f",
                "deflated": True,
                "member": "044-76034-038.dmg",
                "offset": 270462833,
                "sha256": "ae2a7b69aaf9f58f8ebb8c9f10767b597ba5266e591f541dca0f9a56a4d394c6",
                "size": 3303014400,
                "stored_size": 1321525120,
            },
            "system_cryptex_trust_cache": {
                "component": "Cryptex1,SystemTrustCache",
                "content_sha256": "9742f3d566369e6c48f63b3dc0740b06c243d13c6650e25bbf44616e13d6d097",
                "deflated": True,
                "member": "Firmware/044-76034-038.dmg.trustcache",
                "offset": 6749450558,
                "sha256": "ce0baf51e89d850811eff073f1baf75d9b6494b3ea35837f672d9c12d6821b8e",
                "size": 1871,
                "stored_size": 1748,
            },
        },
        "product": "iPad7,1",
        "size": 6835124840,
        "url": "https://updates.cdn-apple.com/2025SpringFCS/fullrestores/122-77166/DCA6CFE8-90E9-4B92-880D-3DE171798E40/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw",
        "version": "17.7.11",
        "volumes": {
            "app_cryptex_image": "044-75188-038.dmg",
            "app_cryptex_uuid": "C0DCFAA8-4E81-49CD-807D-BED3B9B6AE54",
            "os_volume_uuid": "04769D26-2CA0-4825-B443-E2E5F88300F2",
            "system_cryptex_image": "044-76034-038.dmg",
            "system_cryptex_uuid": "8846FE78-2910-4297-AF97-D59BB6F8FC16",
        },
        "xnu": "10063.141.1",
    },
    "24B119": {
        "build": "24B119",
        "derived": {
            "kernel_virt_base": "0xfffffff00307c000",
            "kernelcache_sha256": "250337d45598da0e5cc578413704981368afe92f84b3450dc956e21da1b21bd4",
            "pretranslated": {
                "a64x-cache-pretranslated.bin": "4117978f49cc8ddb2b7d915b6067a3d925b416b79befbc18faa677a0ee0ec9b1",
                "app_cryptex.trustcache": "8c49696d35668a109494d6d3b3560013947f34f93b99b876b266127f0a7d20a7",
                "os.trustcache": "6999dd97b488b0d6c28982ed3d72a91b2e70561e2ae3c93d0df13ee28f2fe2c0",
                "system_cryptex.trustcache": "ac159b6cc99ebba670088b69fbb44b2b503b6ae094a98f0a178eeda85b41156d",
            },
            "quartzcore_metallib_sha256": "342e3ee16a83e7b04837dcdadf4a786020711e3179afcf0ca50a461e7331218b",
        },
        "device_class": "j181ap",
        "ios": "27",
        "members": {
            "app_cryptex": {
                "component": "Cryptex1,AppOS",
                "content_sha256": "8a70ca8f34f8604f436dd3ec23d27aea21a9dc5b28f114acc7ada0ee76df010c",
                "deflated": True,
                "member": "141-08824-244.dmg",
                "offset": 47,
                "sha256": "71b113308b8cbeb77ea5e5b4c3396cb075f94b4d4ab8375837dba34db3aede50",
                "size": 16777216,
                "stored_size": 8962415,
            },
            "app_cryptex_trust_cache": {
                "component": "Cryptex1,AppTrustCache",
                "content_sha256": "618c041eca6b8b7c160864be1d64efa84f93912759a8e241c1d8a66dfde52fb0",
                "deflated": True,
                "member": "Firmware/141-08824-244.dmg.trustcache",
                "offset": 10519920406,
                "sha256": "3c60e7245889088a8d7ab74246712596ee3a39b222fc5035b3ef04147318f95c",
                "size": 863,
                "stored_size": 833,
            },
            "device_tree": {
                "component": "DeviceTree",
                "content_sha256": "e77e065d4368bbce85b0a01228499176d3a7f8ea1968bb7d0b200ea3028c3e6b",
                "deflated": True,
                "member": "Firmware/all_flash/DeviceTree.j181ap.im4p",
                "offset": 10621133195,
                "sha256": "83b909acc045a78a9a0e8892496ba9b379acbe6b8ed9d29251206c064d8c6ebf",
                "size": 36554,
                "stored_size": 36569,
            },
            "kernelcache": {
                "component": "KernelCache",
                "content_sha256": "eb4b0f24a1145c5f7d22b293b74de2947517963c7a4efe229df4b97bdfa9651f",
                "deflated": True,
                "member": "kernelcache.release.ipad12p",
                "offset": 10659917172,
                "sha256": "ce3cda3cb30a8168806fcd6f1cc50d6090fb633572b9976011034903c8b0fc5c",
                "size": 21687326,
                "stored_size": 21668113,
            },
            "os": {
                "aea_key": "base64:HzcLKynvr+LYWwgI8nvik7nKA6RQRdfyGJ+l3hyv4mA=",
                "component": "OS",
                "content_sha256": "4ee01ed973cb11a6e88b4684072cc9bb76b45acd5c18e66a80252aff01125968",
                "deflated": False,
                "member": "141-09394-217.dmg.aea",
                "offset": 8962533,
                "sha256": "939612a319ce0e533688bea0996437b951d47b3383eb9e6b9f114c092d09eaf0",
                "size": 7717519360,
                "stored_size": 7717519360,
            },
            "static_trust_cache": {
                "component": "StaticTrustCache",
                "content_sha256": "c71268adca6fd53512eacbf692558d77aa36c7a8ac8409598c6901465e075361",
                "deflated": True,
                "member": "Firmware/141-09394-217.dmg.aea.trustcache",
                "offset": 10555209215,
                "sha256": "8ac49a3586f46c3f2356bb9aa768a666c03edc927846928831da1fb400a1f639",
                "size": 84601,
                "stored_size": 76659,
            },
            "system_cryptex": {
                "aea_key": "base64:vkMWcw+0u3HwUw8AehkWDt2CyPQCNWpz0Qz7NzGg+M8=",
                "component": "Cryptex1,SystemOS",
                "content_sha256": "d215b0bd1b3a4201d4d20c311e6baf114f808f63ca9a718dce7439b06cc49f83",
                "deflated": False,
                "member": "141-10410-227.dmg.aea",
                "offset": 8103971707,
                "sha256": "423778eee61b0fe78740d86928dc6d023aa9daf8e5a8afddbc64f54a4dcdea51",
                "size": 2415919104,
                "stored_size": 2415919104,
            },
            "system_cryptex_trust_cache": {
                "component": "Cryptex1,SystemTrustCache",
                "content_sha256": "e714a3fcaa20281df406f150339e17277a1e6db79abe6c089ab88bb1d0c0064e",
                "deflated": True,
                "member": "Firmware/141-10410-227.dmg.aea.trustcache",
                "offset": 10555302247,
                "sha256": "444670402cf8bc1c0f6b56c561dfc2d7e5b30beb33dfcaff9dc10a869f25177f",
                "size": 3191,
                "stored_size": 2922,
            },
        },
        "product": "iPad12,1",
        "size": 10681591172,
        "url": "https://updates.cdn-apple.com/2026FallFCS/4ed6b0d9-7721-4690-b843-664305638776/iPad_10.2_2021_27.2_24B119_Restore.ipsw",
        "version": "27.2",
        "volumes": {
            "app_cryptex_image": "141-08824-244.dmg",
            "app_cryptex_uuid": "D8B9F68E-F191-4FCD-9F89-D88A322E9B4D",
            "os_volume_uuid": "D05A9D99-D227-4757-9F58-C32593D3BFA7",
            "system_cryptex_image": "141-10410-227.dmg",
            "system_cryptex_uuid": "ADB8FD34-DA7E-4538-9AAF-4F05EBB09057",
        },
        "xnu": "12377.121.6",
    },
}
