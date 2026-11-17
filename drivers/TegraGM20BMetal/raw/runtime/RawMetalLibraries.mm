// SPDX-License-Identifier: MIT
// A library the device makes of a metallib it reads itself (RawMetalLibrary.mm readLibrary): its twin and its functions'
// are m2v's only once something asks for them. A file, URL, data or bundle library it cannot read fails as Metal's and
// m2v's do. Source, stitching and Core Image's linking are the system's Metal front end's (RawMetalCompiledLibraries.mm).
#import "RawMetalInternal.h"

#include <os/log.h>

// The file Metal would read, mapped as the frontend maps it (NSDataReadingMappedIfSafe): its pages are the file's, never
// this process's footprint
static NSData *metallibFile(NSString *path, NSError **error)
{
    return path ? [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:error] : nil;
}

static NSError *libraryError(MTLLibraryError code, NSString *why)
{
    return [NSError errorWithDomain:MTLLibraryErrorDomain code:code userInfo:@{NSLocalizedDescriptionKey : why}];
}

// Bytes that are no metallib Metal loads (no AIR slice it takes, a function list cut short): nil and Metal's error, as m2v's
// frontend answered them
static id<MTLLibrary> unread(NSString *from, NSError **error)
{
    os_log_error(rawLog(), "raw: %{public}s: no metallib Metal loads in %{public}s", getprogname(),
                 from.UTF8String ?: "data");
    if (error)
        *error = libraryError(MTLLibraryErrorUnsupported,
                              @"This library does not have a compatible AIR slice for this device");
    return nil;
}

static id<MTLLibrary> fileLibrary(id device, NSString *path, NSString *named, NSError **error)
{
    NSError *readError = nil;
    NSData *bytes = metallibFile(path, &readError);
    if (!bytes) {
        if (error)
            *error = readError ?: libraryError(MTLLibraryErrorFileNotFound, @"no library file");
        return nil;
    }
    if (RMLibrary *l = readLibrary(device, named, bytes)) {
        if (error)
            *error = nil;
        return (id<MTLLibrary>)l;
    }
    return unread(path, error);
}

@implementation RMDevice (Libraries)

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if (!url.isFileURL) {
        if (error)
            *error = libraryError(MTLLibraryErrorFileNotFound, @"a file URL is required");
        return nil;
    }
    return fileLibrary(self, url.path, url.path, error);
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    return fileLibrary(self, path, path, error);
}

- (id<MTLLibrary>)newLibraryWithData:(dispatch_data_t)data error:(NSError **)error
{
    const void *bytes = nullptr;
    size_t size = 0;
    dispatch_data_t mapped = data ? dispatch_data_create_map(data, &bytes, &size) : nil;
    NSData *held = mapped ? [[NSData alloc] initWithBytesNoCopy:const_cast<void *>(bytes)
                                                         length:size
                                                    deallocator:^(void *, NSUInteger) {
                                                        (void)mapped;
                                                    }]
                          : nil;
    if (RMLibrary *l = readLibrary(self, nil, held)) {
        if (error)
            *error = nil;
        return (id<MTLLibrary>)l;
    }
    return unread(nil, error);
}

// The library is named as m2v's was (no path for the main bundle's, the bundle's path for another's), so its framework,
// system flag and compile requests are unchanged
- (id<MTLLibrary>)newDefaultLibrary
{
    NSURL *url = [[NSBundle mainBundle] URLForResource:@"default" withExtension:@"metallib"];
    return url ? fileLibrary(self, url.path, nil, nil) : nil;
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    if (!url) {
        if (error)
            *error = libraryError(MTLLibraryErrorFileNotFound, @"the bundle has no default.metallib");
        return nil;
    }
    return fileLibrary(self, url.path, [bundle.bundlePath stringByAppendingString:@"/default.metallib"], error);
}

@end
