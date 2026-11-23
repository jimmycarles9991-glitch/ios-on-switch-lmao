// SPDX-License-Identifier: MIT
// Debug builds: how a render state's creation got its record (obtained.h) and which store the process maps, for the HW
// Test's gpu.compile.shared.
#import "TegraGM20BMetalInternal.h"

#if SWITCH_METAL_DIAGNOSTICS
@implementation SwitchMetalPipeline (CompileProbe)

// how: pack, table (this process's), store (the shader compiler's, read in place), compiled (the compiler made it for
// this request), joined (for another request in flight), stored (the compiler found it stored), none; wait_us and qos: the creating thread's wait for the compiler and its class; ran_class: the compiler's pool band
// it compiled at (compiler_pool.h Band, 4 user-interactive); queue_us, compile_us, answer_us, wake_us, store_us and
// answer_priority: where wait_us went (obtained.h, -1 for a part not measured)
- (NSDictionary *)switchMetalCompile
{
    return @{
        @"how" : @(_obtained.how),
        @"wait_us" : @(_obtained.waitUs),
        @"qos" : @(_obtained.qos),
        @"ran_class" : @((_obtained.ran >> 8) & 0xff),
        @"queue_us" : @(_obtained.queueUs),
        @"compile_us" : @(_obtained.compileUs),
        @"answer_us" : @(_obtained.answerUs),
        @"wake_us" : @(_obtained.wakeUs),
        @"store_us" : @(_obtained.storeUs),
        @"answer_priority" : @(_obtained.answerPriority),
    };
}

@end

@implementation SwitchMetalDevice (CompileProbe)

// The shader compiler's store this process maps: its stack key and pack rules' digest in hex ("" for none)
- (NSString *)switchCompilerStore
{
    return gRM->completion ? @(gRM->completion->compilerStore().c_str()) : @"";
}

@end
#endif
