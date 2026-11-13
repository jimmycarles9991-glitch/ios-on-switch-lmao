// SPDX-License-Identifier: MIT
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>

@interface SwitchSharedEvent : NSObject <MTLSharedEvent>
- (instancetype)initWithOptions:(NSUInteger)options;
- (instancetype)initWithHandle:(MTLSharedEventHandle *)handle;
- (instancetype)initWithMachPort:(mach_port_t)port;
@property (readonly) mach_port_t eventPort;
@property (readonly) uint64_t labelTraceID;
@property (readonly) id IOSurfaceSharedEvent;
@property (readonly) BOOL supportsRollback;
@property (nonatomic, weak) id<MTLDevice> device;
@end
