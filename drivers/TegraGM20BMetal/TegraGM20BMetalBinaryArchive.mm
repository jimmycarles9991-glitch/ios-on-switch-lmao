// SPDX-License-Identifier: MIT
// MTLBinaryArchive: the device's own, a list of pipeline keys (archive_keys.h). Opened by Metal's rules: no URL is an
// empty archive; a file this device wrote holds its keys; a metallib or an Apple GPU binary (archive_keys.h
// archiveFile) holds the states the pack declares for that file (pack.h PackArchive), and nothing else of this GPU's;
// an archive file m2v wrote holds nothing of this GPU's; a file of no archive format, or none, is
// MTLBinaryArchiveErrorInvalidFile. A creation under
// MTLPipelineOptionFailOnBinaryArchiveMiss that lists archives none of which holds its state is nil with the target
// driver's error, before any compile (an M4 Max: AGXMetalG16X code 3), but for a framework's state the pack holds, the
// device's own archive hit (TegraGM20BMetalStates.mm gm20bPipelineFor).
#import "TegraGM20BMetalBinaryArchive.h"
#import "TegraGM20BMetalInternal.h"

#include <mutex>
#import "TegraGM20BMetalPipelineKey.h"
#include "compute_key.h"
#include "device_answers.h"

@interface SwitchMetalBinaryArchive : SwitchMetalObject <MTLBinaryArchive> {
@public
    gm20b::UnfairMutex _lock;
    gm20b::ArchiveKeys _keys;
    const gm20b::PackArchive *_declared;  // the pack's list for the Apple file it was opened from
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

uint64_t gm20bArchiveKey(const std::string &key)
{
    return gm20b::Pack::fnv(key.data(), key.size());
}

static std::string computeKey(MTLComputePipelineDescriptor *d, SwitchMetalFunction *f)
{
    return gm20b::computeArchiveKey(libraryName(f), f->_constants, uint32_t(d.maxTotalThreadsPerThreadgroup),
                                    d.threadGroupSizeIsMultipleOfThreadExecutionWidth);
}

static std::string tileKey(MTLTileRenderPipelineDescriptor *d, SwitchMetalFunction *f)
{
    std::vector<uint32_t> formats;
    for (NSUInteger i = 0; i < 8; i++)
        formats.push_back(uint32_t(d.colorAttachments[i].pixelFormat));
    return gm20b::archiveTileKey(libraryName(f), f->_constants, formats, uint32_t(d.rasterSampleCount),
                                 d.threadgroupSizeMatchesTileSize);
}

static SwitchMetalFunction *own(id<MTLFunction> function)
{
    return [(id)function isKindOfClass:[SwitchMetalFunction class]] ? (SwitchMetalFunction *)function : nil;
}

@implementation SwitchMetalBinaryArchive
@dynamic device;  // SwitchMetalObject's

- (bool)holds:(uint64_t)key
{
    std::lock_guard<gm20b::UnfairMutex> l(_lock);
    return _keys.holds(key) || (_declared && gRM->raw->pack().declares(*_declared, key));
}

- (BOOL)add:(uint64_t)key
{
    std::lock_guard<gm20b::UnfairMutex> l(_lock);
    _keys.add(key);
    return YES;
}

- (BOOL)addRenderPipelineFunctionsWithDescriptor:(MTLRenderPipelineDescriptor *)d error:(NSError **)error
{
    if (!own(d.vertexFunction) || (d.fragmentFunction && !own(d.fragmentFunction)))
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    if (const char *why = gm20bUnsupportedRenderState(d))
        return failed(error, MTLBinaryArchiveErrorCompilationFailure, @(why));
    return [self add:gm20bArchiveKey(metal_pipeline_key(d).UTF8String)];
}

- (BOOL)addComputePipelineFunctionsWithDescriptor:(MTLComputePipelineDescriptor *)d error:(NSError **)error
{
    SwitchMetalFunction *f = own(d.computeFunction);
    if (!f)
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    return [self add:gm20bArchiveKey(computeKey(d, f))];
}

- (BOOL)addTileRenderPipelineFunctionsWithDescriptor:(MTLTileRenderPipelineDescriptor *)d error:(NSError **)error
{
    SwitchMetalFunction *f = own(d.tileFunction);
    if (!f)
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a function of another device");
    return [self add:gm20bArchiveKey(tileKey(d, f))];
}

- (BOOL)addFunctionWithDescriptor:(MTLFunctionDescriptor *)d library:(id<MTLLibrary>)library error:(NSError **)error
{
    if (![(id)library isKindOfClass:[SwitchMetalLibrary class]])
        return failed(error, MTLBinaryArchiveErrorUnexpectedElement, @"a library of another device");
    const std::string key = std::string("f ") + (((SwitchMetalLibrary *)library)->_framework) + " " +
                            (d.name.UTF8String ?: "") + " " + gm20b::constantsText(d.constantValues);
    return [self add:gm20bArchiveKey(key)];
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
        std::lock_guard<gm20b::UnfairMutex> l(_lock);
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

id<MTLBinaryArchive> gm20bNewBinaryArchive(id device, MTLBinaryArchiveDescriptor *d, NSError **error)
{
    SwitchMetalBinaryArchive *archive = [SwitchMetalBinaryArchive new];
    archive->_device = device;
    if (!d.url)
        return archive;
    NSData *file =
        d.url.isFileURL ? [NSData dataWithContentsOfURL:d.url options:NSDataReadingMappedIfSafe error:nil] : nil;
    const auto *bytes = static_cast<const uint8_t *>(file.bytes);
    switch (file ? gm20b::archiveFile(bytes, file.length) : gm20b::ArchiveFile::None) {
    case gm20b::ArchiveFile::Apple:
        archive->_declared = gm20b::declaredBy(gRM->raw->pack(), d.url.path.UTF8String ?: "");
        break;
    case gm20b::ArchiveFile::M2v:
        break;
    case gm20b::ArchiveFile::Keys:
        if (gm20b::ArchiveKeys::parse(bytes, file.length, archive->_keys))
            break;
        [[fallthrough]];
    case gm20b::ArchiveFile::None:
        if (error)
            *error = archiveError(MTLBinaryArchiveErrorInvalidFile,
                                  [NSString stringWithFormat:@"%@ is no archive", d.url.path]);
        return nil;
    }
    return archive;
}

gm20b::ArchiveAnswer gm20bArchiveAnswer(NSArray *archives, MTLPipelineOption options, uint64_t key)
{
    return gm20b::archiveAnswer(options & MTLPipelineOptionFailOnBinaryArchiveMiss, archives.count, [&](size_t i) {
        id archive = archives[i];
        return
            [archive isKindOfClass:[SwitchMetalBinaryArchive class]] && [(SwitchMetalBinaryArchive *)archive holds:key];
    });
}

NSError *gm20bArchiveMissError(NSString *function, const char *pipeline)
{
    NSString *text = [NSString
        stringWithFormat:@"Unable to find function \"%@\" (%s pipeline) in binary archives.", function, pipeline];
    return [NSError errorWithDomain:@(gm20b::answers::kThisKind.driver)
                               code:3
                           userInfo:@{NSLocalizedDescriptionKey : text}];
}

bool gm20bPackHoldsKernels(const std::string &stateKey)
{
    const gm20b::Pack &pack = gRM->raw->pack();
    const gm20b::PackArchive *held = pack.archive(gm20b::kPackHeldKernels);
    return held && pack.declares(*held, gm20b::Pack::fnv(stateKey.data(), stateKey.size()));
}

const gm20b::PackComputeFacts *gm20bPackComputeFacts(const std::string &stateKey)
{
    return gRM->raw->pack().computeFacts(gm20b::Pack::fnv(stateKey.data(), stateKey.size()));
}
