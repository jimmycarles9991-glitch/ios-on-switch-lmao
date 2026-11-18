#pragma once

#if DEVELOPMENT || DEBUG
#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface SwitchImportDiagnostics : NSObject
+ (void)capture:(NSString *)event;
@end

NS_ASSUME_NONNULL_END
#endif
