// SPDX-License-Identifier: MIT
// The build's compile profile (bridge/NativeMetalOffline.rs), which directpack's offline compile makes its translator
// devices of, against the m2v runtime device this Mac's NVK twin opens on the mock GM20B.
// usage: compile-profile <nvk dylib> <cache dir> [--check]
// Without --check it prints the runtime's profile, which drivers/NativeMetal/bridge/compile_profile/<build>.json holds;
// with it, exits 1 naming the difference where the built-in profile is not the runtime's.
#include <cstdio>
#include <cstring>
#include <string>
#include "bridge/NativeMetalDirect.h"

int main(int argc, char **argv)
{
    const bool check = argc == 4 && !strcmp(argv[3], "--check");
    if (argc != 3 && !check) {
        fprintf(stderr, "usage: compile-profile <nvk dylib> <cache dir> [--check]\n");
        return 2;
    }
    NmbDevice *device = nmb_device_open(argv[1], argv[2]);
    if (!device) {
        fprintf(stderr, "compile-profile: no device: %s\n", nmb_last_error());
        return 1;
    }
    int status = 0;
    if (check) {
        if (!nmb_compile_profile_check(device)) {
            fprintf(stderr, "compile-profile: %s\n", nmb_last_error());
            status = 1;
        }
    } else {
        std::string json(nmb_compile_profile_freeze(device, nullptr, 0), '\0');
        if (json.empty() || nmb_compile_profile_freeze(device, json.data(), json.size()) != json.size()) {
            fprintf(stderr, "compile-profile: %s\n", nmb_last_error());
            status = 1;
        } else {
            printf("%s\n", json.c_str());
        }
    }
    nmb_device_release(device);
    return status;
}
