// SPDX-License-Identifier: MIT
// The device's binary archives are m2v's (its twin states list them, its functions are added to them), opened by Metal's rules:
// m2v's own reader's, and an Apple GPU binary as an archive holding nothing for this GPU (raw_archive.h). What this GPU has
// compiled is the pack and the record cache, which every state's lookup reads whatever archive its descriptor lists.
#import "RawMetalInternal.h"

#include <fcntl.h>
#include <unistd.h>
#include "raw_archive.h"

static bool appleGpuBinaryAt(NSURL *url)
{
    const int fd = url.isFileURL ? open(url.fileSystemRepresentation, O_RDONLY | O_CLOEXEC) : -1;
    if (fd < 0)
        return false;
    uint8_t head[8];
    const ssize_t got = read(fd, head, sizeof head);
    close(fd);
    return got > 0 && raw::appleGpuBinary(head, size_t(got));
}

@implementation RMDevice (Archives)

// RenderBox's load_library_archive logs a precondition failure where this is nil, and draws without the archive
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    if (d.url && appleGpuBinaryAt(d.url)) {
        MTLBinaryArchiveDescriptor *empty = [d copy];
        empty.url = nil;
        return [gRM->fallback newBinaryArchiveWithDescriptor:empty error:error];
    }
    return [gRM->fallback newBinaryArchiveWithDescriptor:d error:error];
}

@end
