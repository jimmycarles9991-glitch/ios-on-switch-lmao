// SPDX-License-Identifier: MIT
// computeeq's raw side: computeframe's raw and pack modes alone (no frontend), over the eq capture
#include <cstdio>
#include <cstring>
#include "../../raw/test/computeframe.h"

int main(int argc, char **argv)
{
    if (argc >= 7 && (!strcmp(argv[1], "raw") || !strcmp(argv[1], "pack")))
        return computeframeRaw(argv);
    fprintf(stderr, "usage: computeframe raw <pack.bin> <nvk log> <programs.bin> <threads x,y,z> <groups x,y,z>\n"
                    "       computeframe pack <pack.bin> - <key> <threads x,y,z> <groups x,y,z>\n");
    return 2;
}
