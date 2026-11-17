// SPDX-License-Identifier: MIT
#import "RawMetalFunctionTags.h"
#import "RawMetalInternal.h"
#include "RawComplete.h"
#include "raw_function_attributes.h"
#include "raw_function_tags.h"

// Swift's collection bridges require Metal's concrete metadata classes.
@interface SwitchMetalFunctionConstant : MTLFunctionConstant
@property (nonatomic, copy) NSString *name;
@property (nonatomic) MTLDataType type;
@property (nonatomic) NSUInteger index;
@property (nonatomic) BOOL required;
@end
@implementation SwitchMetalFunctionConstant
@synthesize name = switch_name, type = switch_type, index = switch_index, required = switch_required;
@end

@interface SwitchMetalVertexAttribute : MTLVertexAttribute
@property (nonatomic, copy) NSString *name;
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic) MTLDataType attributeType;
@property (nonatomic, getter=isActive) BOOL active;
@property (nonatomic, getter=isPatchData) BOOL patchData;
@property (nonatomic, getter=isPatchControlPointData) BOOL patchControlPointData;
@end
@implementation SwitchMetalVertexAttribute
@synthesize name = switch_name, attributeIndex = switch_index, attributeType = switch_type, active = switch_active,
            patchData = switch_patch_data, patchControlPointData = switch_patch_control_point_data;
@end

@interface SwitchMetalAttribute : MTLAttribute
@property (nonatomic, copy) NSString *name;
@property (nonatomic) NSUInteger attributeIndex;
@property (nonatomic) MTLDataType attributeType;
@property (nonatomic, getter=isActive) BOOL active;
@property (nonatomic, getter=isPatchData) BOOL patchData;
@property (nonatomic, getter=isPatchControlPointData) BOOL patchControlPointData;
@end
@implementation SwitchMetalAttribute
@synthesize name = switch_name, attributeIndex = switch_index, attributeType = switch_type, active = switch_active,
            patchData = switch_patch_data, patchControlPointData = switch_patch_control_point_data;
@end

static bool tagsOf(RMFunction *f, raw::FunctionTags &tags)
{
    if (f->_source && f->_source->bytes &&
        raw::functionTags(f->_source->bytes, f->_source->size, f->_libraryName, tags))
        return true;
    os_log_error(rawLog(), "raw: %{public}s: no public metadata of function %{public}s in its library", getprogname(),
                 f->_libraryName.c_str());
    return false;
}

NSDictionary<NSString *, MTLFunctionConstant *> *rmFunctionConstants(RMFunction *f)
{
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    raw::FunctionTags tags;
    if (f->_specialized || !tagsOf(f, tags))
        return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
    if (!tags.constants)
        os_log_error(rawLog(),
                     "raw: %{public}s: function %{public}s lists a constant of a type or flag Metal's answer was "
                     "not measured for: none given",
                     getprogname(), f->_libraryName.c_str());
    for (const raw::FunctionConstantTag &c : tags.constants.value_or(std::vector<raw::FunctionConstantTag>{})) {
        SwitchMetalFunctionConstant *constant = [SwitchMetalFunctionConstant new];
        constant.name = @(c.name.c_str());
        constant.type = MTLDataType(c.type);
        constant.index = c.index;
        constant.required = c.required;
        out[constant.name] = constant;
    }
    return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
}

static NSArray *attributeObjects(const std::vector<raw::FunctionAttributeTag> &list, bool stageInput)
{
    NSMutableArray *attributes = [NSMutableArray arrayWithCapacity:list.size()];
    for (const raw::FunctionAttributeTag &a : list) {
        id attribute = stageInput ? [SwitchMetalAttribute new] : [SwitchMetalVertexAttribute new];
        [attribute setName:@(a.name.c_str())];
        [attribute setAttributeIndex:a.index];
        [attribute setAttributeType:MTLDataType(a.type)];
        [attribute setActive:a.active];
        [attribute setPatchData:a.patchData];
        [attribute setPatchControlPointData:a.controlPointData];
        [attributes addObject:attribute];
    }
    return attributes;
}

// The AIR hash of the library function `f` was made from; false with `why`
static bool airHashOf(RMFunction *f, raw::AirHash &out, std::string &why)
{
    const void *bytes = nullptr;
    size_t length = 0;
    NSString *libraryFunction = nil;
    std::map<std::string, raw::AirHash> hashes;
    if (!rmFunctionSource((id<MTLFunction>)f, &bytes, &length, &libraryFunction) ||
        !raw::metallibAirHashes(static_cast<const uint8_t *>(bytes), length, hashes) ||
        !hashes.count(libraryFunction.UTF8String ?: ""))
        return why = "the function's library has no AIR of it", false;
    out = hashes[libraryFunction.UTF8String];
    return true;
}

// A function made with constant values that has attributes: the shader compiler's answer of its specialized AIR
static NSArray *specializedAttributes(RMFunction *f, bool stageInput)
{
    raw::AirHash air;
    std::string blob, why;
    raw::AttributeList vertex, stage;
    if (airHashOf(f, air, why)) {
        const std::string key = raw::functionAttributesKey(air, f->_constants, f.name.UTF8String ?: "");
        if (!gRM->completion)
            why = "no shader compiler comes this boot";
        else if (gRM->completion->attributes(f, key, blob, why) && raw::decodeAttributeLists(blob, vertex, stage)) {
            const raw::AttributeList &list = stageInput ? stage : vertex;
            return list ? attributeObjects(*list, stageInput) : nil;
        }
    }
    os_log_error(rawLog(), "raw: %{public}s: no attributes of specialized function %{public}s: %{public}s, nil given",
                 getprogname(), f.name.UTF8String ?: "?", why.c_str());
    return nil;
}

bool rmBufferEncoders(RMFunction *f, raw::BufferEncoders &out, std::string &why)
{
    raw::AirHash air;
    std::string blob;
    if (!airHashOf(f, air, why))
        return false;
    if (!gRM->completion)
        return why = "no shader compiler comes this boot", false;
    if (!gRM->completion->bufferEncoders(f, raw::bufferEncodersKey(air, f->_constants, f->_specialized), blob, why))
        return false;
    if (!raw::decodeBufferEncoders(blob, out))
        return why = "the shader compiler's answer does not decode", false;
    return true;
}

NSArray *rmFunctionAttributes(RMFunction *f, bool stageInput)
{
    raw::FunctionTags tags;
    if (!tagsOf(f, tags))
        return nil;
    if (!tags.attributes) {
        os_log_error(rawLog(),
                     "raw: %{public}s: function %{public}s's attributes are of a kind Metal's answer was not "
                     "measured for: nil given",
                     getprogname(), f->_libraryName.c_str());
        return nil;
    }
    if (tags.attributes->empty() || (!stageInput && f->_type != MTLFunctionTypeVertex))
        return nil;
    return f->_specialized ? specializedAttributes(f, stageInput) : attributeObjects(*tags.attributes, stageInput);
}
