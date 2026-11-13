// SPDX-License-Identifier: MIT
// MTLBinaryArchive: the device's own, a list of pipeline keys (raw_archive_keys.h). Opened by Metal's rules: no URL is an
// empty archive; a file this device wrote holds its keys; a metallib or an Apple GPU binary (raw_archive_keys.h
// archiveFile) holds the states the pack declares for that file (raw_pack.h PackArchive), and nothing else of this GPU's;
// an archive file m2v wrote holds nothing of this GPU's; a file of no archive format, or none, is
// MTLBinaryArchiveErrorInvalidFile. A creation under
// MTLPipelineOptionFailOnBinaryArchiveMiss that lists archives none of which holds its state is nil with the target
// driver's error, before any compile (an M4 Max: AGXMetalG16X code 3), but for a framework's state the pack holds, the
// device's own archive hit (RawMetalStates.mm rmPipelineFor).
#import "RawMetalBinaryArchive.h"
#import "RawMetalInternal.h"

#include <mutex>
#include "NativeBridgeMetalFrontend.h"
#import "RawPipelineKey.h"
#include "raw_compute_key.h"
#include "raw_device_answers.h"

@interface RMBinaryArchive : RMObject <MTLBinaryArchive> {
@public
    raw::Mutex _lock;
    raw::ArchiveKeys _keys;
    const raw::PackArchive *_declared;  // the pack's list for the Apple file it was opened from
}
@end

static NSError *archiveError(MTLBinaryArchiveError code, NSString *text)
{
    return [NSError errorWithDomain:MTLBinaryArchiveDomain code:code userInfo:@{NSLocalizedDescriptionKey : text}];
}

static BOOL failed(NSError **error, MTLBinaryArchiveError code, NSString *text)
{
    if (error)
        *error = archiveError(code, text);
    return NO;
}

uint64_t rmArchiveKey(const std::string &key)
{
    return raw::Pack::fnv(key.data(), key.size());
}

static NSString *archiveLibraryName(id<MTLFunction> function)
{
    if ([(id)function isKindOfClass:[RMFunction class]])
        return [NSString stringWithUTF8String:libraryName((RMFunction *)function).c_str()];
    return native_bridge_metal_function_library_name(function) ?: function.name;
}

static std::string computeKey(MTLComputePipelineDescriptor *d, RMFunction *f)
{
    return raw::computeArchiveKey(archiveLibraryName((id<MTLFunction>)f).UTF8String ?: "", f->_constants,
                                  uint32_t(d.maxTotalThreadsPerThreadgroup),
                                  d.threadGroupSizeIsMultipleOfThreadExecutionWidth);
}

static std::string tileKey(MTLTileRenderPipelineDescriptor *d, RMFunction *f)
{
    std::vector<uint32_t> formats;
    for (NSUInteger i = 0; i < 8; i++)
        formats.push_back(uint32_t(d.colorAttachments[i].pixelFormat));
    return raw::archiveTileKey(archiveLibraryName((id<MTLFunction>)f).UTF8String ?: "", f->_constants, formats,
                               uint32_t(d.rasterSampleCount), d.threadgroupSizeMatchesTileSize);
}

static RMFunction *own(id<MTLFunction> function)
{
    return [(id)function isKindOfClass:[RMFunction class]] ? (RMFunction *)function : nil;
}

@implementation RMBinaryArchive
@dynamic device;  // RMObject's

+ (Class)twinClass
{
    return NSClassFromString(@"NBBinaryArchive");
}

- (bool)holds:(uint64_t)key
{
    std::lock_guard<raw::Mutex> l(_lock);
    return _keys.holds(key) || (_declared && gRM->raw->pack().declares(*_declared, key));
}

- (BOOL)add:(uint64_t)key
{
    std::lock_guard<raw::Mutex> l(_lock);
    _keys.add(key);
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
    if (!own(d.vertexFunction) || (d.fragmentFunction && !own(d.fragmentFunction)))
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    if (const char *why = rmUnsupportedRenderState(d))
        return failed(error, MTLBinaryArchiveErrorCompilationFailure, @(why));
    return [self add:rmArchiveKey(raw_metal_pipeline_key(d).UTF8String)];
}

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error
{
    RMFunction *f = own(d.computeFunction);
    if (!f)
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    if (const char *why = rmUnsupportedComputeState(d))
        return failed(error, MTLBinaryArchiveErrorCompilationFailure, @(why));
    return [self add:rmArchiveKey(computeKey(d, f))];
}

- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)d error:(NSError **)error
{
    RMFunction *f = own(d.tileFunction);
    if (!f)
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    return [self add:rmArchiveKey(tileKey(d, f))];
}

- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)d library:(id<MTLLibrary>)library error:(NSError **)error
{
    if (![(id)library isKindOfClass:[RMLibrary class]])
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a library of another device");
    const std::string key = std::string("f ") + (((RMLibrary *)library)->_framework) + " " + (d.name.UTF8String ?: "") +
                            " " + raw::constantsText(d.constantValues);
    return [self add:rmArchiveKey(key)];
}

// Mesh pipelines and dynamic libraries are no states of this device (refused by design at their creation)
- (BOOL)addMeshRenderPipelineFunctionsWithDescriptor:(MTLMeshRenderPipelineDescriptor *)d error:(NSError **)error
{
    (void)d;
    return failed(error, MTLBinaryArchiveErrorCompilationFailure, @"mesh pipelines are not this device's");
}

- (BOOL)addLibraryWithDescriptor:(MTLStitchedLibraryDescriptor *)d error:(NSError **)error
{
    (void)d;
    return failed(error, MTLBinaryArchiveErrorCompilationFailure, @"dynamic libraries are not this device's");
}

- (BOOL)serializeToURL:(NSURL *)url error:(NSError **)error
{
    std::vector<uint8_t> bytes;
    {
        std::lock_guard<raw::Mutex> l(_lock);
        bytes = _keys.serialized();
    }
    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    return [data writeToURL:url options:NSDataWritingAtomic error:error];
}

- (NSUInteger)options
{
    return 0;
}

@end

id<MTLBinaryArchive> rmNewBinaryArchive(id device, MTLBinaryArchiveDescriptor *d, NSError **error)
{
    RMBinaryArchive *archive = [RMBinaryArchive new];
    archive->_device = device;
    if (!d.url)
        return archive;
    NSData *file =
        d.url.isFileURL ? [NSData dataWithContentsOfURL:d.url options:NSDataReadingMappedIfSafe error:nil] : nil;
    const auto *bytes = static_cast<const uint8_t *>(file.bytes);
    switch (file ? raw::archiveFile(bytes, file.length) : raw::ArchiveFile::None) {
    case raw::ArchiveFile::Apple:
        archive->_declared = raw::declaredBy(gRM->raw->pack(), d.url.path.UTF8String ?: "");
        break;
    case raw::ArchiveFile::M2v:
        break;
    case raw::ArchiveFile::Keys:
        if (raw::ArchiveKeys::parse(bytes, file.length, archive->_keys))
            break;
        [[fallthrough]];
    case raw::ArchiveFile::None:
        if (error)
            *error = archiveError(MTLBinaryArchiveErrorInvalidFile,
                                  [NSString stringWithFormat:@"%@ is no archive", d.url.path]);
        return nil;
    }
    return archive;
}

raw::ArchiveAnswer rmArchiveAnswer(NSArray *archives, MTLPipelineOption options, uint64_t key)
{
    return raw::archiveAnswer(options & MTLPipelineOptionFailOnBinaryArchiveMiss, archives.count, [&](size_t i) {
        id archive = archives[i];
        return [archive isKindOfClass:[RMBinaryArchive class]] && [(RMBinaryArchive *)archive holds:key];
    });
}

NSError *rmArchiveMissError(NSString *function, const char *pipeline)
{
    NSString *text = [NSString
        stringWithFormat:@"Unable to find function \"%@\" (%s pipeline) in binary archives.", function, pipeline];
    return [NSError errorWithDomain:@(raw::answers::kThisKind.driver)
                               code:3
                           userInfo:@{NSLocalizedDescriptionKey : text}];
}

static bool missed(raw::ArchiveAnswer answer, NSString *function, const char *pipeline, NSError **error)
{
    if (answer != raw::ArchiveAnswer::Missed)
        return false;
    if (error)
        *error = rmArchiveMissError(function, pipeline);
    return true;
}

bool rmPackHoldsKernels(const std::string &stateKey)
{
    const raw::Pack &pack = gRM->raw->pack();
    const raw::PackArchive *held = pack.archive(raw::kPackHeldKernels);
    return held && pack.declares(*held, raw::Pack::fnv(stateKey.data(), stateKey.size()));
}

const raw::PackComputeFacts *rmPackComputeFacts(const std::string &stateKey)
{
    return gRM->raw->pack().computeFacts(raw::Pack::fnv(stateKey.data(), stateKey.size()));
}

bool rmTileArchiveAnswer(MTLTileRenderPipelineDescriptor *twin, MTLTileRenderPipelineDescriptor *d,
                         MTLPipelineOption &options, NSError **error)
{
    RMFunction *f = own(d.tileFunction);
    const raw::ArchiveAnswer answer =
        f ? rmArchiveAnswer(d.binaryArchives, options, rmArchiveKey(tileKey(d, f))) : raw::ArchiveAnswer::NotAsked;
    twin.binaryArchives = nil;
    options &= ~MTLPipelineOptionFailOnBinaryArchiveMiss;
    return !missed(answer, f.name, "tile", error);
}
