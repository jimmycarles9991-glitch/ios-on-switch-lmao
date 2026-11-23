// SPDX-License-Identifier: MIT
#import "TegraGM20BMetalFunctionTags.h"
#import "TegraGM20BMetalInternal.h"
#include "TegraGM20BMetalComplete.h"
#include "function_attributes.h"
#include "function_tags.h"

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

static bool tagsOf(SwitchMetalFunction *f, gm20b::FunctionTags &tags)
{
    if (f->_source && f->_source->bytes &&
        gm20b::functionTags(f->_source->bytes, f->_source->size, f->_libraryName, tags))
        return true;
    os_log_error(gm20bLog(), "SwitchMetal: %{public}s: no public metadata of function %{public}s in its library",
                 getprogname(), f->_libraryName.c_str());
    return false;
}

// The function's constants in its metallib's order; none for a specialized function, whose constants have their values
static NSArray<MTLFunctionConstant *> *constantList(SwitchMetalFunction *f)
{
    NSMutableArray *out = [NSMutableArray array];
    gm20b::FunctionTags tags;
    if (f->_specialized || !tagsOf(f, tags))
        return out;
    if (!tags.constants)
        os_log_error(
            gm20bLog(),
            "SwitchMetal: %{public}s: function %{public}s lists a constant of a type or flag Metal's answer was "
            "not measured for: none given",
            getprogname(), f->_libraryName.c_str());
    for (const gm20b::FunctionConstantTag &c : tags.constants.value_or(std::vector<gm20b::FunctionConstantTag>{})) {
        SwitchMetalFunctionConstant *constant = [SwitchMetalFunctionConstant new];
        constant.name = @(c.name.c_str());
        constant.type = MTLDataType(c.type);
        constant.index = c.index;
        constant.required = c.required;
        [out addObject:constant];
    }
    return out;
}

NSDictionary<NSString *, MTLFunctionConstant *> *gm20bFunctionConstants(SwitchMetalFunction *f)
{
    NSMutableDictionary *out = [NSMutableDictionary dictionary];
    for (MTLFunctionConstant *constant in constantList(f))
        out[constant.name] = constant;
    return (NSDictionary<NSString *, MTLFunctionConstant *> *)out;
}

// -[_MTLFunctionInternal functionConstants], the list functionConstantsDictionary is made of: MTLFunctionConstantValues
// serializes a function's values in its order, and Metal's tools and VFX read it of any function
@interface SwitchMetalFunction (FunctionConstants)
- (NSArray<MTLFunctionConstant *> *)functionConstants;
@end
@implementation SwitchMetalFunction (FunctionConstants)

- (NSArray<MTLFunctionConstant *> *)functionConstants
{
    return constantList(self);
}

@end

static NSArray *attributeObjects(const std::vector<gm20b::FunctionAttributeTag> &list, bool stageInput)
{
    NSMutableArray *attributes = [NSMutableArray arrayWithCapacity:list.size()];
    for (const gm20b::FunctionAttributeTag &a : list) {
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
static bool airHashOf(SwitchMetalFunction *f, gm20b::AirHash &out, std::string &why)
{
    if (!f->_air)
        return why = "the function's library has no AIR of it", false;
    out = *f->_air;
    return true;
}

// A function made with constant values that has attributes: the shader compiler's answer of its specialized AIR
static NSArray *specializedAttributes(SwitchMetalFunction *f, bool stageInput)
{
    gm20b::AirHash air;
    std::string blob, why;
    gm20b::AttributeList vertex, stage;
    if (airHashOf(f, air, why)) {
        const std::string key = gm20b::functionAttributesKey(air, f->_constants, f.name.UTF8String ?: "");
        if (!gRM->completion)
            why = "no shader compiler comes this boot";
        else if (gRM->completion->attributes(f, key, blob, why) && gm20b::decodeAttributeLists(blob, vertex, stage)) {
            const gm20b::AttributeList &list = stageInput ? stage : vertex;
            return list ? attributeObjects(*list, stageInput) : nil;
        }
    }
    os_log_error(gm20bLog(),
                 "SwitchMetal: %{public}s: no attributes of specialized function %{public}s: %{public}s, nil given",
                 getprogname(), f.name.UTF8String ?: "?", why.c_str());
    return nil;
}

bool gm20bBufferEncoders(SwitchMetalFunction *f, gm20b::BufferEncoders &out, std::string &why)
{
    gm20b::AirHash air;
    std::string blob;
    if (!airHashOf(f, air, why))
        return false;
    if (!gRM->completion)
        return why = "no shader compiler comes this boot", false;
    if (!gRM->completion->bufferEncoders(f, gm20b::bufferEncodersKey(air, f->_constants, f->_specialized), blob, why))
        return false;
    if (!gm20b::decodeBufferEncoders(blob, out))
        return why = "the shader compiler's answer does not decode", false;
    return true;
}

NSArray *gm20bFunctionAttributes(SwitchMetalFunction *f, bool stageInput)
{
    gm20b::FunctionTags tags;
    if (!tagsOf(f, tags))
        return nil;
    if (!tags.attributes) {
        os_log_error(gm20bLog(),
                     "SwitchMetal: %{public}s: function %{public}s's attributes are of a kind Metal's answer was not "
                     "measured for: nil given",
                     getprogname(), f->_libraryName.c_str());
        return nil;
    }
    if (tags.attributes->empty() || (!stageInput && f->_type != MTLFunctionTypeVertex))
        return nil;
    return f->_specialized ? specializedAttributes(f, stageInput) : attributeObjects(*tags.attributes, stageInput);
}
