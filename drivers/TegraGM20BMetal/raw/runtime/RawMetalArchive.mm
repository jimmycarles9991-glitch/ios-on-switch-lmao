// SPDX-License-Identifier: MIT
#import "RawMetalBinaryArchive.h"
#import "RawMetalInternal.h"

@implementation RMDevice (Archives)

// RenderBox's load_library_archive logs a precondition failure where this is nil, and draws without the archive
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    return rmNewBinaryArchive(self, d, error);
}

@end
