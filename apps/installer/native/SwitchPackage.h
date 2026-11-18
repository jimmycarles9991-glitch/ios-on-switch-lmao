#pragma once

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface SwitchPackage : NSObject
@property (nonatomic, readonly, copy) NSString *name;
@property (nonatomic, readonly, copy) NSString *identifier;
@property (nonatomic, readonly, copy) NSString *version;
@property (nonatomic, readonly, copy) NSString *minimumOSVersion;
@property (nonatomic, readonly) uint64_t expandedSize;
@property (nonatomic, readonly, nullable) NSData *iconData;
+ (nullable instancetype)inspectURL:(NSURL *)url systemVersion:(NSString *)systemVersion error:(NSError **)error;
@end

NS_ASSUME_NONNULL_END
