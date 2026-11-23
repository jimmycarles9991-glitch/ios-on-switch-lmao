// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalBinaryArchive.h"
#import "TegraGM20BMetalInternal.h"

@implementation SwitchMetalDevice (Archives)

// RenderBox's load_library_archive logs a precondition failure where this is nil, and draws without the archive
- (id<MTLBinaryArchive>)newBinaryArchiveWithDescriptor:(MTLBinaryArchiveDescriptor *)d error:(NSError **)error
{
    return gm20bNewBinaryArchive(self, d, error);
}

@end
