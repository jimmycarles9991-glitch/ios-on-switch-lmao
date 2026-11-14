// SPDX-License-Identifier: MIT
#include "raw_surface_note.h"

#include <gtest/gtest.h>
#include <cstring>

namespace {

IOSurfaceRef makeSurface(int width, int height)
{
    int bytes = 4;
    OSType format = 'BGRA';
    CFNumberRef w = CFNumberCreate(nullptr, kCFNumberIntType, &width);
    CFNumberRef h = CFNumberCreate(nullptr, kCFNumberIntType, &height);
    CFNumberRef b = CFNumberCreate(nullptr, kCFNumberIntType, &bytes);
    CFNumberRef f = CFNumberCreate(nullptr, kCFNumberIntType, &format);
    const void *keys[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat,
                          kIOSurfaceName};
    const void *values[] = {w, h, b, f, CFSTR("CA layer")};
    CFDictionaryRef d =
        CFDictionaryCreate(nullptr, keys, values, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(d);
    CFRelease(d);
    for (CFNumberRef n : {w, h, b, f})
        CFRelease(n);
    return s;
}

TEST(RawSurfaceNote, TakesScalarsAndCallersAndHoldsNoReference)
{
    IOSurfaceRef s = makeSurface(1280, 720);
    ASSERT_NE(s, nullptr);
    const raw::SurfaceNote note = raw::surfaceNote(s, __builtin_frame_address(0));
    EXPECT_EQ(note.surface, IOSurfaceGetID(s));
    EXPECT_EQ(note.width, 1280u);
    EXPECT_EQ(note.height, 720u);
    EXPECT_EQ(note.format, uint32_t('BGRA'));
    EXPECT_NE(note.site.pc[0], 0u);

    char line[512];
    raw::describeSurfaceNote(note, line, sizeof line);
    EXPECT_NE(strstr(line, "1280x720 'BGRA'"), nullptr) << line;
    EXPECT_NE(strstr(line, "use count 0 in use 0"), nullptr) << line;
    EXPECT_NE(strstr(line, "name CA layer"), nullptr) << line;

    CFRelease(s);
    raw::describeSurfaceNote(note, line, sizeof line);
    EXPECT_NE(strstr(line, "not found"), nullptr) << line;
}

TEST(RawSurfaceNote, ATargetNoteNamesItsDescriptorAndCallers)
{
    raw::TargetNote n;
    n.width = 1792;
    n.height = 1472;
    n.format = 80;
    n.usage = 5;
    n.storage = 2;
    n.samples = 1;
    n.type = 2;
    n.site = switch_frame::callers(__builtin_frame_address(0));
    char line[384];
    raw::describeTargetNote(n, line, sizeof line);
    EXPECT_NE(strstr(line, "raw: compositor target 1792x1472 format 80 usage 0x5 storage 2 samples 1 type 2 at "),
              nullptr)
        << line;
    EXPECT_NE(strstr(line, "raw_surface_note_test+0x"), nullptr) << line;
}

TEST(RawSurfaceNote, TwentyNotesASecondAThread)
{
    int allowed = 0;
    for (int i = 0; i < 100; i++)
        allowed += raw::noteAllowed();
    EXPECT_GE(allowed, 20);
    EXPECT_LE(allowed, 40);
}

}  // namespace
