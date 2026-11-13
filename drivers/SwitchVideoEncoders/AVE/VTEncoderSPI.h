// SPDX-License-Identifier: BSD-2-Clause-Patent
// VideoToolbox's encoder plug-in interface (exported by VideoToolbox and CoreMedia, not in the SDK headers). The CMBase
// part is VTDecoderSPI.h's. The layout and signatures are 21H461's own: H264H9.videoencoder (Apple's AVE encoder, in
// the dyld cache) keeps C++ names, so its H3H264VideoEncoderVTable at 0x22ab2a4c8 reads {0, CMBaseClass (version 1,
// invalidate/finalize/copyProperty/setProperty at +0x18/+0x20/+0x30/+0x38), VTVideoEncoderClass at 0x22ab2a548
// (version 3): StartSession(VTVideoEncoder*, VTVideoEncoderSession*, CMVideoDimensions, CFTypeRef) +8,
// EncodeFrame(VTVideoEncoder*, VTVideoEncoderFrame*, CVBuffer*, CMTime, CMTime, CFDictionary const*, unsigned*) +0x10,
// CompleteFrames(VTVideoEncoder*, CMTime) +0x18, CopySupportedPropertyDictionary +0x20, SetProperties +0x28,
// PrepareToEncodeFrames +0x38, BeginPass/EndPass +0x40/+0x48}. VideoToolbox's own MuxedAlpha encoder declares
// version 1 with only the first four slots. Factories take (codec type, allocator, out) (_TestIPBVideoEncoder_
// CreateInstance); VTEncoderSessionCreateVideoFormatDescription takes (session, codec, width, height, extensions,
// out) and VTEncoderSessionEmitEncodedFrame (session, frame, status, info flags, sample buffer) (both disassembled).
//
// Tile sessions (VTTileCompressionSession, the HEIC grid encoder CMPhoto drives), the same on 21H461 and 24B119:
// VideoToolbox takes an encoder only when the class version, read as 64 bits, is 3 or more and +0x88 is set
// (24B119 VTTileCompressionSessionCreateWithOptions 0x18f2ea3d0; 21H461 VTTileCompressionSessionCreate +0x320), and
// calls StartTileSession(encoder, session, encoder specification, tile size, allocator) +0x88,
// PrepareToEncodeTiles(encoder, Boolean) +0x90 (VTTileCompressionSessionPrepareToEncodeTiles 0x18f2ea89c),
// EncodeTile(encoder, tile, source, origin, size, options, info flags) +0x98 (VTTileCompressionSessionEncodeTile
// 0x18f2eaa64) and CompleteTiles(encoder) +0xa0 (0x18f2e9cec), as Apple's AVE HEVC class (version 4,
// gsc_sAVE_Plugin_HEVC 0x2d5c00e60) fills them. VTTileEncoderSessionSetTileAttributes(session, tile size, pixel
// buffer attributes) stores what VTTileCompressionSessionCopyProperty answers for VideoEncoderPixelBufferAttributes,
// and the size PrepareToEncodeTiles hands the client back (0x18f20a694, 0x18f2ea8b0); SetTileEncodeRequirements
// (session, canvas attributes, requirements) those for CanvasPixelBufferAttributes and TileEncoderRequirements
// (0x18f20a824). VTEncoderSessionEmitEncodedTile(session, tile, status, info flags, sample buffer) finds the tile
// EncodeTile entered and calls the client's output with the sample as it is (0x18f20a6e0);
// VTTileEncoderSessionCreateVideoFormatDescription is CMVideoFormatDescriptionCreate with the session's allocator
// (0x18f2eabb0).
#ifndef VT_ENCODER_SPI_H
#define VT_ENCODER_SPI_H
#include "../../SwitchVideoDecoders/AVD/VTDecoderSPI.h"

typedef struct OpaqueVTVideoEncoderSession *VTVideoEncoderSession;
typedef struct OpaqueVTVideoEncoderFrame *VTVideoEncoderFrame;
typedef struct OpaqueVTTileEncoderSession *VTTileEncoderSession;
typedef struct OpaqueVTEncoderTile *VTEncoderTile;
typedef CMBaseObjectRef VTVideoEncoderRef;

typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoEncoderRef, VTVideoEncoderSession, CMVideoDimensions, CFTypeRef);
    OSStatus (*encodeFrame)(VTVideoEncoderRef, VTVideoEncoderFrame, CVImageBufferRef, CMTime, CMTime, CFDictionaryRef,
                            VTEncodeInfoFlags *);
    OSStatus (*completeFrames)(VTVideoEncoderRef, CMTime);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoEncoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoEncoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoEncoderRef, CFAllocatorRef, CFDictionaryRef *);
    OSStatus (*prepareToEncodeFrames)(VTVideoEncoderRef);
    void *beginPass, *endPass;
    void *reserved[7];
    OSStatus (*startTileSession)(VTVideoEncoderRef, VTTileEncoderSession, CFDictionaryRef, VTInt32Size, CFAllocatorRef);
    OSStatus (*prepareToEncodeTiles)(VTVideoEncoderRef, Boolean);
    OSStatus (*encodeTile)(VTVideoEncoderRef, VTEncoderTile, CVImageBufferRef, VTInt32Point, VTInt32Size,
                           CFDictionaryRef, VTEncodeInfoFlags *);
    OSStatus (*completeTiles)(VTVideoEncoderRef);
    void *encodeMultiImageFrame;
    void *reserved2[2];
} VTPluginEncoderClass;

typedef struct {
    const void *reserved;
    const VTPluginBaseClass *baseClass;
    const VTPluginEncoderClass *encoderClass;
} VTPluginEncoderVTable;

_Static_assert(offsetof(VTPluginEncoderClass, encodeFrame) == 0x10, "VTVideoEncoderClass");
_Static_assert(offsetof(VTPluginEncoderClass, copySupportedPropertyDictionary) == 0x20, "VTVideoEncoderClass");
_Static_assert(offsetof(VTPluginEncoderClass, prepareToEncodeFrames) == 0x38, "VTVideoEncoderClass");
_Static_assert(offsetof(VTPluginEncoderClass, startTileSession) == 0x88, "VTVideoEncoderClass v3");
_Static_assert(offsetof(VTPluginEncoderClass, encodeTile) == 0x98, "VTVideoEncoderClass v3");
_Static_assert(offsetof(VTPluginEncoderClass, completeTiles) == 0xa0, "VTVideoEncoderClass v3");
_Static_assert(sizeof(VTPluginEncoderClass) == 0xc0, "VTVideoEncoderClass");

typedef OSStatus (*VTEncoderCreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, VTVideoEncoderRef *out);

extern OSStatus VTRegisterVideoEncoderWithInfo(FourCharCode codecType, CFDictionaryRef info,
                                               VTEncoderCreateInstance create);
extern CMBaseClassID VTVideoEncoderGetClassID(void);
extern CMBaseObjectRef VTVideoEncoderGetCMBaseObject(VTVideoEncoderRef encoder);
extern OSStatus VTEncoderSessionSetPixelBufferAttributes(VTVideoEncoderSession session, CFDictionaryRef attributes);
extern OSStatus VTEncoderSessionCreateVideoFormatDescription(VTVideoEncoderSession session, CMVideoCodecType codec,
                                                             int32_t width, int32_t height, CFDictionaryRef extensions,
                                                             CMVideoFormatDescriptionRef *out);
extern OSStatus VTEncoderSessionEmitEncodedFrame(VTVideoEncoderSession session, VTVideoEncoderFrame frame,
                                                 OSStatus status, VTEncodeInfoFlags flags, CMSampleBufferRef sample);

extern OSStatus VTTileEncoderSessionSetTileAttributes(VTTileEncoderSession session, VTInt32Size tileSize,
                                                      CFDictionaryRef pixelBufferAttributes);
extern OSStatus VTTileEncoderSessionSetTileEncodeRequirements(VTTileEncoderSession session,
                                                              CFDictionaryRef canvasAttributes,
                                                              CFDictionaryRef requirements);
extern OSStatus VTTileEncoderSessionCreateVideoFormatDescription(VTTileEncoderSession session, CMVideoCodecType codec,
                                                                 int32_t width, int32_t height,
                                                                 CFDictionaryRef extensions,
                                                                 CMVideoFormatDescriptionRef *out);
extern OSStatus VTEncoderSessionEmitEncodedTile(VTTileEncoderSession session, VTEncoderTile tile, OSStatus status,
                                                VTEncodeInfoFlags flags, CMSampleBufferRef sample);
extern const CFStringRef kVTTileEncoderRequirement_HorizontalOffsetAlignment;
extern const CFStringRef kVTTileEncoderRequirement_VerticalOffsetAlignment;
extern const CFStringRef kVTTileEncoderRequirement_EncoderSupportsSourcePadding;

extern const CFStringRef kVTVideoEncoder_IsHardwareAccelerated;
extern const CFStringRef kVTVideoEncoder_EncoderNameKey;
extern const CFStringRef kVTVideoEncoderList_EncoderID;

#endif
