// SPDX-License-Identifier: BSD-2-Clause-Patent
// VideoToolbox's decoder plug-in interface (exported by VideoToolbox and CoreMedia, not in the SDK headers).
// The shapes are those WebKit's VP9 decoder and others declare; the layout is checked against 21H461 itself:
// H264SW.videocodec's vtable at 0x231e9c248 = {0, CMBaseClass at 0x231e9c260 (version 1, derived storage 0x480 at
// +8, finalize/copyDebugDescription/copyProperty/setProperty at +0x20..+0x38), VTVideoDecoderClass at 0x231e9c2c8
// (version 3, startSession +8, decodeFrame +0x10, copySupportedPropertyDictionary +0x18, canAccept +0x30)}. Naturally
// aligned on arm64 (not the pack(4) of the x86 headers), so every slot is 8 bytes.
#ifndef VT_DECODER_SPI_H
#define VT_DECODER_SPI_H
#include <stddef.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>

typedef struct OpaqueCMBaseObject *CMBaseObjectRef;
typedef struct OpaqueCMBaseClass *CMBaseClassID;
typedef struct OpaqueVTVideoDecoderSession *VTVideoDecoderSession;
typedef struct OpaqueVTVideoDecoderFrame *VTVideoDecoderFrame;
typedef CMBaseObjectRef VTVideoDecoderRef;
typedef struct OpaqueVTTileDecoderSession *VTTileDecoderSession;

typedef struct {
    uint32_t version;
    size_t derivedStorageSize;
    Boolean (*equal)(CMBaseObjectRef, CMBaseObjectRef);
    OSStatus (*invalidate)(CMBaseObjectRef);
    void (*finalize)(CMBaseObjectRef);
    CFStringRef (*copyDebugDescription)(CMBaseObjectRef);
    OSStatus (*copyProperty)(CMBaseObjectRef, CFStringRef, CFAllocatorRef, void *);
    OSStatus (*setProperty)(CMBaseObjectRef, CFStringRef, CFTypeRef);
    OSStatus (*notificationBarrier)(CMBaseObjectRef);
    const void *protocolTable;
} VTPluginBaseClass;

typedef struct {
    uint32_t version;
    OSStatus (*startSession)(VTVideoDecoderRef, VTVideoDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeFrame)(VTVideoDecoderRef, VTVideoDecoderFrame, CMSampleBufferRef, VTDecodeFrameFlags,
                            VTDecodeInfoFlags *);
    OSStatus (*copySupportedPropertyDictionary)(VTVideoDecoderRef, CFDictionaryRef *);
    OSStatus (*setProperties)(VTVideoDecoderRef, CFDictionaryRef);
    OSStatus (*copySerializableProperties)(VTVideoDecoderRef, CFAllocatorRef, CFDictionaryRef *);
    Boolean (*canAcceptFormatDescription)(VTVideoDecoderRef, CMVideoFormatDescriptionRef);
    OSStatus (*finishDelayedFrames)(VTVideoDecoderRef);
    // Version 3 (Apple's A10X HEVC decoder, H264H8 0x231f34028, fills these two): a tile session's start, and one tile
    // decoded into `destination` with its top-left at `offset`. VideoToolbox passes the tile's place in the image and
    // its size as well; Apple's decoder uses neither.
    OSStatus (*startTileSession)(VTVideoDecoderRef, VTTileDecoderSession, CMVideoFormatDescriptionRef);
    OSStatus (*decodeTile)(VTVideoDecoderRef, int64_t frameID, CMSampleBufferRef, VTInt32Point tile, VTInt32Size size,
                           CVPixelBufferRef destination, VTInt32Point offset, VTDecodeFrameFlags, VTDecodeInfoFlags *);
    OSStatus (*finishDelayedTiles)(VTVideoDecoderRef);
} VTPluginDecoderClass;

typedef struct {
    const void *reserved;
    const VTPluginBaseClass *baseClass;
    const VTPluginDecoderClass *decoderClass;
} VTPluginDecoderVTable;

_Static_assert(offsetof(VTPluginBaseClass, derivedStorageSize) == 0x8, "CMBaseClass");
_Static_assert(offsetof(VTPluginBaseClass, finalize) == 0x20, "CMBaseClass");
_Static_assert(offsetof(VTPluginBaseClass, setProperty) == 0x38, "CMBaseClass");
_Static_assert(offsetof(VTPluginDecoderClass, decodeFrame) == 0x10, "VTVideoDecoderClass");
_Static_assert(offsetof(VTPluginDecoderClass, canAcceptFormatDescription) == 0x30, "VTVideoDecoderClass");
_Static_assert(offsetof(VTPluginDecoderClass, startTileSession) == 0x40, "VTVideoDecoderClass v3");
_Static_assert(offsetof(VTPluginDecoderClass, decodeTile) == 0x48, "VTVideoDecoderClass v3");
_Static_assert(offsetof(VTPluginDecoderClass, finishDelayedTiles) == 0x50, "VTVideoDecoderClass v3");
_Static_assert(sizeof(VTInt32Point) == 8 && sizeof(VTInt32Size) == 8, "VTBase.h's, one register each by value");

typedef OSStatus (*VTDecoderCreateInstance)(FourCharCode codecType, CFAllocatorRef allocator, VTVideoDecoderRef *out);

extern OSStatus VTRegisterVideoDecoder(FourCharCode codecType, VTDecoderCreateInstance create);
extern CMBaseClassID VTVideoDecoderGetClassID(void);
extern OSStatus CMDerivedObjectCreate(CFAllocatorRef allocator, const void *vtable, CMBaseClassID classID,
                                      CMBaseObjectRef *out);
extern void *CMBaseObjectGetDerivedStorage(CMBaseObjectRef object);
extern const void *CMBaseObjectGetVTable(CMBaseObjectRef object);  // a VTPluginDecoderVTable for a decoder
extern CMBaseObjectRef VTVideoDecoderGetCMBaseObject(VTVideoDecoderRef decoder);
// The registry's choice for a codec under a decoder specification (21H461 VideoToolbox, exported): with an out-pointer
// it runs the candidates' factories in rating order until one succeeds; with none it only answers whether one exists.
// A decoder specification for 'vp09' is refused unless it carries this (true): VideoToolbox's selection returns no
// decoder otherwise (21H461 0x19789ee98); VTIsHardwareDecodeSupported sets it in Fig servers and entitled tasks.
extern const CFStringRef kVTVideoDecoderSpecification_AllowVP9Decode;
// A decoder's own ID (its info's CMClassImplementationID + "." + the codec) names it for a session; registry info
// key IsHardwareAccelerated decides whether hardware queries count it. Left out (VTRegisterVideoDecoder), 21H461 reads
// it as true and 24B119 as false.
extern const CFStringRef kVTVideoDecoderSpecification_DecoderID;
extern const CFStringRef kVTVideoDecoder_IsHardwareAccelerated;
// The registry's first sort key (21H461 _vtSortRegistryItemsByRating, 0x197abe13c): a higher kVTVideoDecoderInfo_Rating
// (CFNumber, default 0) ranks before IsHardwareAccelerated is even compared.
extern const CFStringRef kVTVideoDecoderInfo_Rating;
extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode codecType, CFDictionaryRef info,
                                               VTDecoderCreateInstance create);
extern OSStatus VTSelectAndCreateVideoDecoderInstance(FourCharCode codecType, CFAllocatorRef allocator,
                                                      CFDictionaryRef specification, VTVideoDecoderRef *out);
extern OSStatus VTDecoderSessionSetPixelBufferAttributes(VTVideoDecoderSession session, CFDictionaryRef attributes);
extern CVPixelBufferPoolRef VTDecoderSessionGetPixelBufferPool(VTVideoDecoderSession session);
// A pixel buffer for `frame` from the session's pool, or, in a session another one feeds, the buffer VideoToolbox
// remembered for that session's frame (24B119 0x18f00861c; 21H461 exports it too, 0x1978dbd58).
extern OSStatus VTDecoderSessionCreatePixelBuffer(VTVideoDecoderSession session, VTVideoDecoderFrame frame,
                                                  CVPixelBufferRef *out);
extern OSStatus VTDecoderSessionEmitDecodedFrame(VTVideoDecoderSession session, VTVideoDecoderFrame frame,
                                                 OSStatus status, VTDecodeInfoFlags flags, CVImageBufferRef image);
// A tile session's answer for frameID; a NULL image means the destination the tile was decoded into.
extern OSStatus VTDecoderSessionEmitDecodedTile(VTTileDecoderSession session, int64_t frameID, OSStatus status,
                                                VTDecodeInfoFlags flags, CVImageBufferRef image);
// What the caller must make the destination canvas like (pixel buffer attributes) and the tiles it may send.
extern OSStatus VTTileDecoderSessionSetTileDecodeRequirements(VTTileDecoderSession session,
                                                              CFDictionaryRef pixelBufferAttributes,
                                                              CFDictionaryRef requirements);
extern const CFStringRef kVTTileDecoderRequirement_TileByteAlignment;
extern const CFStringRef kVTTileDecoderRequirement_MinimumTileWidth;
extern const CFStringRef kVTTileDecoderRequirement_MaximumTileWidth;
extern const CFStringRef kVTTileDecoderRequirement_MinimumTileHeight;
extern const CFStringRef kVTTileDecoderRequirement_MaximumTileHeight;
extern const CFStringRef kVTTileDecoderRequirement_MaximumCanvasRowBytes;
extern const CFStringRef kVTTileDecoderRequirement_MinimumDecodableWidth;
extern const CFStringRef kVTTileDecoderRequirement_MinimumDecodableHeight;
extern const CFStringRef kVTTileDecoderRequirement_TileWidthPixelAlignment;
extern const CFStringRef kVTTileDecoderRequirement_TileHeightPixelAlignment;
extern const CFStringRef kVTTileDecoderRequirement_MaximumCanvasWidth;
extern const CFStringRef kVTTileDecoderRequirement_MaximumCanvasHeight;
extern const CFStringRef kVTTileDecoderRequirement_DecoderCanClipEdges;

#endif
