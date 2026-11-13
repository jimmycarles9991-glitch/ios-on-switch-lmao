// SPDX-License-Identifier: MIT
// A library the device makes of a metallib it reads itself (RawMetalLibrary.mm readLibrary): its twin and its functions'
// are m2v's only once something asks for them. Source, stitching and Core Image's linking are m2v's from the start.
#import "RawMetalInternal.h"

// The file Metal would read, mapped as the frontend maps it (NSDataReadingMappedIfSafe): its pages are the file's, never
// this process's footprint
static NSData *metallibFile(NSString *path)
{
    return path ? [NSData dataWithContentsOfFile:path options:NSDataReadingMappedIfSafe error:nil] : nil;
}

@implementation RMDevice (Libraries)

- (id<MTLLibrary>)newLibraryWithURL:(NSURL *)url error:(NSError **)error
{
    if (url.isFileURL)
        if (RMLibrary *l = readLibrary(self, url.path, metallibFile(url.path))) {
            if (error)
                *error = nil;
            return (id<MTLLibrary>)l;
        }
    id<MTLDevice> m2v = RM_M2V("a library from a URL raw does not read");
    return (id<MTLLibrary>)wrapLibrary(self, [m2v newLibraryWithURL:url error:error], url.path);
}

- (id<MTLLibrary>)newLibraryWithFile:(NSString *)path error:(NSError **)error
{
    if (RMLibrary *l = readLibrary(self, path, metallibFile(path))) {
        if (error)
            *error = nil;
        return (id<MTLLibrary>)l;
    }
    id<MTLDevice> m2v = RM_M2V("a library from a file raw does not read");
    return (id<MTLLibrary>)wrapLibrary(self, [m2v newLibraryWithFile:path error:error], path);
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
    return (id<MTLLibrary>)wrapLibrary(
        self, [RM_M2V("a library from data raw does not read") newLibraryWithData:data error:error], nil);
}

// The library is named as m2v's was (no path for the main bundle's, the bundle's path for another's), so its framework,
// system flag and compile requests are unchanged
- (id<MTLLibrary>)newDefaultLibrary
{
    NSURL *url = [[NSBundle mainBundle] URLForResource:@"default" withExtension:@"metallib"];
    if (RMLibrary *l = readLibrary(self, nil, metallibFile(url.path)))
        return (id<MTLLibrary>)l;
    return (id<MTLLibrary>)wrapLibrary(
        self, [RM_M2V("the main bundle's default library raw does not read") newDefaultLibrary], nil);
}

- (id<MTLLibrary>)newDefaultLibraryWithBundle:(NSBundle *)bundle error:(NSError **)error
{
    NSURL *url = [bundle URLForResource:@"default" withExtension:@"metallib"];
    NSString *path = [bundle.bundlePath stringByAppendingString:@"/default.metallib"];
    if (RMLibrary *l = readLibrary(self, path, metallibFile(url.path))) {
        if (error)
            *error = nil;
        return (id<MTLLibrary>)l;
    }
    id<MTLDevice> m2v = RM_M2V("a bundle's default library raw does not read");
    return (id<MTLLibrary>)wrapLibrary(self, [m2v newDefaultLibraryWithBundle:bundle error:error], path);
}

- (id<MTLLibrary>)newLibraryWithSource:(NSString *)source options:(MTLCompileOptions *)options error:(NSError **)error
{
    id<MTLDevice> m2v = RM_M2V("a library from source");
    return (id<MTLLibrary>)wrapLibrary(self, [m2v newLibraryWithSource:source options:options error:error], nil);
}

@end
