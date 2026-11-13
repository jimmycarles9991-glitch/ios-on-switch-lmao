// SPDX-License-Identifier: MIT
// selcover: who answers every selector a kind's system code sends to a Metal object (drivers/NativeMetal/abi/kinds/<kind>.json,
// "sent") on the object this process's raw device hands out:
//   raw     a method of the facade's class (the device: of the raw device class itself, not one it inherits);
//   zero    the facade answers it zero (answerZero), as m2v's object would: neither class has a method for it;
//   m2v     m2v's class has a method the facade's lacks: an encoder or command buffer records it for m2v's replay, any other
//           facade object answers it zero with a fault (answerMissing), and the raw device class forwards it to the m2v
//           device, which it opens for it (RawMetalM2V.mm);
//   m2vobj  the device hands out m2v's own object for the protocol (argument encoders, events, fences, binary archives);
//   base    the device only: _MTLDevice's own method on the iPad (the kind's table, "base"), which the host does not have
//           (NMB_DEVICE_BASE_NSOBJECT);
//   none    nothing answers it (no object of the protocol here, or a device selector no class has).
// Every line that is not raw or zero is a gap. Gaps print per kind and object; the run fails on a gap the baseline lacks and on a
// baseline line that is no longer one. Then every zero-argument selector the facade answers zero is sent to its object, and
// every zero-argument method of an m2v class its facade object lacks (none a kind sends: m2v's own accessors): the rates line
// must count the first answered zero, the second missing (tw=../f), and no object may have made a twin.
// Then the raw device's capability answers against m2v's device (selcover_answers.h), and last the API inventory
// (selcover_inventory.h): the run fails where it differs from raw/test/inventory.tsv. SELCOVER_WRITE=1
// writes the baseline and the inventory instead; SELCOVER_STRICT=1 is m2v's deletion gate: it fails on any inventory row
// raw neither answers, reads nor refuses.
// usage: selcover <pack> <cache> <nvk dylib> <default.metallib> <tree> <tree>/drivers/NativeMetal/abi/kinds/<kind>.json...
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#import <objc/runtime.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include "../runtime/RawMetal.h"
#include "NativeBridgeMetalFrontend.h"
#include "bridge/NativeMetalObjects.h"
#include "selcover_answers.h"
#include "selcover_inventory.h"

namespace {

void missing(const char *, const char *)
{}

// Whether `cls` answers `sel` with a method of its own or an ancestor's below NSObject
bool answers(Class cls, SEL sel)
{
    Method m = cls ? class_getInstanceMethod(cls, sel) : nullptr;
    if (!m)
        return false;
    Method root = class_getInstanceMethod([NSObject class], sel);
    return !root || method_getImplementation(root) != method_getImplementation(m);
}

// The facade's classes: RM*, the device class, and the kernel events the device makes itself (SwitchSharedEvent)
bool facadeClass(Class cls)
{
    const char *name = class_getName(cls);
    return !strncmp(name, "RM", 2) || !strcmp(name, "RawGM20BMetalDevice") || !strcmp(name, "SwitchSharedEvent");
}

struct Object {
    id object;
    Class twin;  // the m2v class that answers what the facade's class forwards (nil: none)
    bool bare;   // made without the m2v object it carries on the device
};

Class twinClassOf(id object)
{
    if (Ivar carried = class_getInstanceVariable(object_getClass(object), "_twin"))
        if (id twin = object_getIvar(object, carried))
            return object_getClass(twin);
    if (object_getClass(object) == NSClassFromString(@"RMCommandBuffer"))
        return NSClassFromString(@"NBCommandBuffer");
    Class meta = object_getClass(object);
    return [meta respondsToSelector:@selector(twinClass)] ? [meta performSelector:@selector(twinClass)] : nil;
}

struct Objects {
    std::map<std::string, Object> byProtocol;

    void put(const char *protocol, id object, Class twin = nil)
    {
        byProtocol[protocol] = {object, object ? (twin ?: twinClassOf(object)) : nil, twin != nil};
    }

    bool make(id<MTLDevice> dev, NSString *metallib)
    {
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                      width:64
                                                                                     height:64
                                                                                  mipmapped:NO];
        td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> texture = [dev newTextureWithDescriptor:td];
        id<MTLCommandQueue> queue = [dev newCommandQueue];
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        id<MTLRenderCommandEncoder> render = [cb renderCommandEncoderWithDescriptor:pass];
        [render endEncoding];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit endEncoding];
        id<MTLComputeCommandEncoder> compute = [cb computeCommandEncoder];
        [compute endEncoding];
        NSError *error = nil;
        id<MTLLibrary> library = [dev newLibraryWithURL:[NSURL fileURLWithPath:metallib] error:&error];
        id<MTLFunction> function =
            library.functionNames.count ? [library newFunctionWithName:library.functionNames[0]] : nil;
        MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
        ds.depthCompareFunction = MTLCompareFunctionLess;
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.size = 1 << 20;
        MTLArgumentDescriptor *argument = [MTLArgumentDescriptor argumentDescriptor];
        argument.dataType = MTLDataTypePointer;
        put("MTLDevice", dev);
        put("MTLCommandQueue", queue);
        put("MTLCommandBuffer", cb);
        put("MTLRenderCommandEncoder", render);
        put("MTLBlitCommandEncoder", blit);
        put("MTLComputeCommandEncoder", compute);
        put("MTLTexture", texture);
        put("MTLBuffer", [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared]);
        put("MTLSamplerState", [dev newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]]);
        put("MTLDepthStencilState", [dev newDepthStencilStateWithDescriptor:ds]);
        put("MTLLibrary", library);
        put("MTLFunction", function);
        put("MTLHeap", [dev newHeapWithDescriptor:hd]);
        put("MTLSharedEvent", [dev newSharedEvent]);
        put("MTLFence", [dev newFence]);
        put("MTLArgumentEncoder", [dev newArgumentEncoderWithArguments:@[ argument ]]);
        put("MTLBinaryArchive", [dev newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error]);
        MTLIndirectCommandBufferDescriptor *icb = [MTLIndirectCommandBufferDescriptor new];
        icb.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
        icb.inheritPipelineState = YES;
        icb.inheritBuffers = YES;
        id<MTLIndirectCommandBuffer> commands = [dev newIndirectCommandBufferWithDescriptor:icb
                                                                            maxCommandCount:1
                                                                                    options:0];
        put("MTLIndirectCommandBuffer", commands);
        put("MTLIndirectRenderCommand", [commands indirectRenderCommandAtIndex:0]);
        MTLIndirectCommandBufferDescriptor *dispatches = [MTLIndirectCommandBufferDescriptor new];
        dispatches.commandTypes =
            MTLIndirectCommandTypeConcurrentDispatch | MTLIndirectCommandTypeConcurrentDispatchThreads;
        dispatches.inheritPipelineState = NO;
        dispatches.inheritBuffers = NO;
        put("MTLIndirectComputeCommand",
            [[dev newIndirectCommandBufferWithDescriptor:dispatches maxCommandCount:1
                                                 options:0] indirectComputeCommandAtIndex:0]);
        // Made only from a pack state or a kernel the host corpus lacks: the facade's classes, with the m2v class each carries
        put("MTLRenderPipelineState", class_createInstance(NSClassFromString(@"RMPipeline"), 0),
            NSClassFromString(@"NBRenderPipeline"));
        put("MTLComputePipelineState", class_createInstance(NSClassFromString(@"RMComputeState"), 0),
            NSClassFromString(@"NBComputePipeline"));
        bool all = true;
        for (const auto &[protocol, o] : byProtocol)
            if (!o.object) {
                fprintf(stderr, "selcover: no %s\n", protocol.c_str());
                all = false;
            }
        return all;
    }
};

const char *who(const Object &o, SEL sel, NSDictionary *base)
{
    if (!o.object)
        return "none";
    Class cls = object_getClass(o.object);
    if (!facadeClass(cls))
        return "m2vobj";
    if (strcmp(class_getName(cls), "RawGM20BMetalDevice") == 0) {
        Class inherited = class_getSuperclass(cls);
        Method own = class_getInstanceMethod(cls, sel);
        if (own && method_getImplementation(own) == _objc_msgForward)
            return "m2v";  // a forwarder: m2v's device class's method, the m2v device opened for it (RawMetalM2V.mm)
        if (own && (!answers(inherited, sel) ||
                    method_getImplementation(own) != class_getMethodImplementation(inherited, sel)))
            return "raw";
        if (answers(inherited, sel))
            return "m2v";
        return base[NSStringFromSelector(sel)] ? "base" : "none";
    }
    if (answers(cls, sel))
        return "raw";
    return o.twin && [o.twin instancesRespondToSelector:sel] ? "m2v" : "zero";
}

// The rates line after one more command buffer (the line needs one), its counters taken
void rates(id<MTLCommandQueue> queue, char *line, size_t size)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    [cb commit];
    [cb waitUntilCompleted];
    *line = 0;
    raw_metal_rates(line, size);
}

// Sends each zero-argument selector of `which` to its object, the answer checked zero; how many, or -1 on a nonzero answer. An
// RMTwinObject's go to the m2v object it carries, as before: not sent.
int send(const std::map<std::string, Object> &objects, const std::set<std::pair<std::string, std::string>> &which)
{
    int sent = 0;
    for (const auto &[protocol, selector] : which) {
        const Object &o = objects.at(protocol);
        SEL sel = sel_registerName(selector.c_str());
        if (strchr(selector.c_str(), ':') || class_getInstanceMethod(object_getClass(o.object), sel) ||
            [o.object isKindOfClass:NSClassFromString(@"RMTwinObject")])
            continue;
        NSMethodSignature *signature = [o.object methodSignatureForSelector:sel];
        if (!signature) {
            printf("NO SIGNATURE -[%s %s]\n", object_getClassName(o.object), selector.c_str());
            return -1;
        }
        NSInvocation *invocation = [NSInvocation invocationWithMethodSignature:signature];
        invocation.selector = sel;
        [invocation invokeWithTarget:o.object];
        uint8_t answer[64] = {}, zero[64] = {};
        if (const NSUInteger n = signature.methodReturnLength; n && n <= sizeof answer) {
            [invocation getReturnValue:answer];
            if (memcmp(answer, zero, n)) {
                printf("NOT ZERO -[%s %s]\n", object_getClassName(o.object), selector.c_str());
                return -1;
            }
        }
        sent++;
    }
    return sent;
}

int counter(const char *line, const char *after, const char *name)
{
    const char *from = strstr(line, after), *at = from ? strstr(from, name) : nullptr;
    return at ? atoi(at + strlen(name)) : -1;
}

bool sendUnanswered(const std::map<std::string, Object> &objects,
                    const std::set<std::pair<std::string, std::string>> &zeros,
                    const std::set<std::pair<std::string, std::string>> &missing)
{
    char line[4096];
    id<MTLCommandQueue> queue = objects.at("MTLCommandQueue").object;
    rates(queue, line, sizeof line);
    const int zeroed = send(objects, zeros), unanswered = send(objects, missing);
    rates(queue, line, sizeof line);
    const int answered = counter(line, " zero=", "="), counted = counter(line, " tw=", "/f");
    // the m2v objects a depth-stencil state, library and function carry from their creation
    static const std::set<std::string> carried = {"MTLDepthStencilState", "MTLLibrary", "MTLFunction"};
    unsigned twins = 0;
    for (const auto &[protocol, o] : objects) {
        Class cls = object_getClass(o.object);
        if (Ivar twin = facadeClass(cls) ? class_getInstanceVariable(cls, "_twin") : nullptr)
            twins += object_getIvar(o.object, twin) && !carried.count(protocol);
    }
    printf(
        "sent %d selectors the facade answers zero (%d counted) and %d it lacks (%d counted missing); twins made: %u\n",
        zeroed, answered, unanswered, counted, twins);
    return zeroed >= 0 && unanswered >= 0 && answered == zeroed && counted == unanswered && !twins;
}

// Methods of the m2v class each facade object stands in for that the facade's class does not answer and no kind sends (m2v's own
// accessors), but an encoder's or command buffer's (recorded for m2v)
std::set<std::pair<std::string, std::string>> m2vOnly(const std::map<std::string, Object> &objects,
                                                      const std::set<std::pair<std::string, std::string>> &sent)
{
    std::set<std::pair<std::string, std::string>> out;
    for (const auto &[protocol, o] : objects) {
        if (!o.object || !o.twin || o.bare || !facadeClass(object_getClass(o.object)))
            continue;
        std::set<std::string> methods;
        for (Class c = o.twin; c && c != [NSObject class]; c = class_getSuperclass(c)) {
            unsigned count = 0;
            Method *list = class_copyMethodList(c, &count);
            for (unsigned i = 0; i < count; i++)
                methods.insert(sel_getName(method_getName(list[i])));
            free(list);
        }
        for (const std::string &m : methods)
            if (m[0] != '.' && m != "dealloc" && !answers(object_getClass(o.object), sel_registerName(m.c_str())) &&
                !sent.count({protocol, m}) && protocol.find("Encoder") == std::string::npos &&
                protocol != "MTLCommandBuffer")
                out.insert({protocol, m});
    }
    return out;
}

// Lines of the inventory that differ from the file's (written instead with `write`)
unsigned inventoryDrift(const std::vector<inventory::Row> &rows, const std::string &path, bool write)
{
    std::vector<std::string> now, had;
    for (const auto &r : rows)
        now.push_back(r.line());
    if (write) {
        std::ofstream out(path);
        out << "# raw's API inventory, written by raw/test/selcover (SELCOVER_WRITE=1; selcover_inventory.h says what "
               "each\n"
               "# column holds): container, item, selector, m2v, sent by, raw\n"
            << inventory::totals(rows);
        for (const std::string &l : now)
            out << l << "\n";
        return 0;
    }
    for (const auto &f : inventory::tsv(path)) {
        std::string l;
        for (size_t i = 0; i < f.size(); i++)
            l += (i ? "\t" : "") + f[i];
        had.push_back(l);
    }
    const std::set<std::string> a(now.begin(), now.end()), b(had.begin(), had.end());
    unsigned differ = 0;
    for (const std::string &l : now)
        if (!b.count(l) && differ++ < 20)
            printf("INVENTORY + %s\n", l.c_str());
    for (const std::string &l : had)
        if (!a.count(l) && differ++ < 20)
            printf("INVENTORY - %s\n", l.c_str());
    return differ;
}

void writeBaseline(const std::set<std::string> &gaps, const std::string &path)
{
    std::ofstream out(path);
    out << "# selcover's gaps: <kind> <protocol> <selector> <m2v|m2vobj|base|none>, every sent selector raw does not "
           "answer "
           "itself or\n# answer zero (raw/test/selcover.mm says who each is). A line goes when raw answers it; a new "
           "gap "
           "fails the run.\n";
    for (const std::string &g : gaps)
        out << g << "\n";
}

}  // namespace

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 7) {
            fprintf(stderr, "usage: selcover <pack> <cache> <nvk dylib> <default.metallib> <tree> <kind json>...\n");
            return 2;
        }
        setenv("NMB_DEVICE_BASE_NSOBJECT", "1", 1);
        id<MTLDevice> m2v = native_bridge_metal_open(argv[3], argv[2], 0, missing);
        NSString *why = nil;
        id<MTLDevice> dev = m2v ? raw_metal_device_open(argv[1], 0, m2v, &why) : nil;
        Objects objects;
        if (!dev || !objects.make(dev, @(argv[4]))) {
            fprintf(stderr, "selcover: no device or objects (%s)\n", m2v ? why.UTF8String : nmb_last_error());
            return 1;
        }
        const std::string tree = argv[5], test = tree + "/drivers/TegraGM20BMetal/raw/test/";
        const bool write = getenv("SELCOVER_WRITE") != nullptr;
        std::set<std::string> gaps, baseline;
        std::set<std::pair<std::string, std::string>> zeros, sentAll;
        std::map<std::pair<std::string, std::string>, std::string> sentBy;
        NSMutableDictionary *bases = [NSMutableDictionary dictionary];
        NSString *text = [NSString stringWithContentsOfFile:@((test + "selgaps.txt").c_str())
                                                   encoding:NSUTF8StringEncoding
                                                      error:nil];
        for (NSString *line in [text componentsSeparatedByString:@"\n"])
            if (line.length && ![line hasPrefix:@"#"])
                baseline.insert(line.UTF8String);
        for (int k = 6; k < argc; k++) {
            NSDictionary *kind = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:@(argv[k])]
                                                                 options:0
                                                                   error:nil];
            NSString *name = kind[@"kind"];
            NSDictionary *base = kind[@"MTLDevice"][@"methods"];
            [bases addEntriesFromDictionary:base];
            NSDictionary<NSString *, NSDictionary *> *sent = kind[@"sent"];
            std::map<std::string, unsigned> totals;
            for (NSString *protocol in [sent.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                const auto it = objects.byProtocol.find(protocol.UTF8String);
                const Object o = it == objects.byProtocol.end() ? Object{nil, nil, false} : it->second;
                unsigned open = 0;
                for (NSString *selector in [sent[protocol].allKeys sortedArrayUsingSelector:@selector(compare:)]) {
                    sentAll.insert({protocol.UTF8String, selector.UTF8String});
                    std::string &by = sentBy[{protocol.UTF8String, selector.UTF8String}];
                    by += (by.empty() ? "" : ",") + std::string(name.UTF8String);
                    const char *how = who(o, sel_registerName(selector.UTF8String),
                                          [protocol isEqualToString:@"MTLDevice"] ? base : nil);
                    totals[how]++;
                    if (!strcmp(how, "zero"))
                        zeros.insert({protocol.UTF8String, selector.UTF8String});
                    if (!strcmp(how, "raw") || !strcmp(how, "zero"))
                        continue;
                    gaps.insert(std::string(name.UTF8String) + " " + protocol.UTF8String + " " + selector.UTF8String +
                                " " + how);
                    open++;
                }
                if (open)
                    printf("%s %s (%s): %u of %lu not raw's\n", name.UTF8String, protocol.UTF8String,
                           o.object ? object_getClassName(o.object) : "-", open, (unsigned long)sent[protocol].count);
            }
            printf("%s:", name.UTF8String);
            for (const auto &[how, count] : totals)
                printf(" %s %u", how.c_str(), count);
            printf("\n");
        }
        unsigned added = 0, gone = 0;
        for (const std::string &g : gaps)
            if (!write && !baseline.count(g)) {
                printf("NEW GAP %s\n", g.c_str());
                added++;
            }
        for (const std::string &b : baseline)
            if (!write && !gaps.count(b)) {
                printf("NO LONGER A GAP (remove it from the baseline) %s\n", b.c_str());
                gone++;
            }
        const bool zerosOk = sendUnanswered(objects.byProtocol, zeros, m2vOnly(objects.byProtocol, sentAll));
        const auto rows = inventory::make(
            tree + "/third_party/m2v/scripts/metal-coverage/coverage.tsv", test + "refused.txt",
            tree + "/drivers/TegraGM20BMetal/raw/runtime", sentBy, [&](const std::string &protocol, SEL sel) {
                const auto it = objects.byProtocol.find(protocol);
                return std::string(who(it == objects.byProtocol.end() ? Object{nil, nil, false} : it->second, sel,
                                       protocol == "MTLDevice" ? bases : nil));
            });
        const unsigned drift = inventoryDrift(rows, test + "inventory.tsv", write);
        if (write)
            writeBaseline(gaps, test + "selgaps.txt");
        const unsigned open = inventory::open(rows);
        printf("inventory: %zu rows, %u neither raw's, read nor refused\n%s", rows.size(), open,
               inventory::totals(rows).c_str());
        unsigned compared = 0;
        const unsigned answersDiffer = answers::differ(dev, m2v, compared);
        printf("device answers: %u compared with m2v's device, %u differ\n", compared, answersDiffer);
        const bool strict = getenv("SELCOVER_STRICT") && open;
        const bool pass = !added && !gone && zerosOk && !drift && !strict && !answersDiffer;
        printf(
            "%s: selcover (%u new gaps, %u baseline lines answered now, %u inventory lines differ, %u device answers "
            "differ%s%s)\n",
            pass ? "PASS" : "FAIL", added, gone, drift, answersDiffer,
            zerosOk ? "" : ", unanswered selectors miscounted", strict ? ", the deletion gate's rows open" : "");
        return pass ? 0 : 1;
    }
}
