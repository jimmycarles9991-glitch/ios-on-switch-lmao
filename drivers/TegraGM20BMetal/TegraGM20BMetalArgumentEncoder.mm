// SPDX-License-Identifier: GPL-2.0-or-later OR LGPL-3.0-or-later
// MTLArgumentEncoder over a layout of argument descriptors, or of a function's buffer as its AIR lays it out (the
// shader compiler's answer, buffer_encoders.h), whose writes and refusals are gm20b::ArgumentEncoder's
// (argument_encoder.h). Each argument is the 8 bytes a program could write itself: a buffer's GPU address plus its
// offset, any other object's gpuResourceID (resource_id.h); a constant is the program's own bytes
// (constantDataAtIndex:). A refusal writes nothing and is one line.
#import "TegraGM20BMetalFunctionTags.h"
#import "TegraGM20BMetalInternal.h"
#include "argument_encoder.h"

// MTLDataTypeDepthStencilState (iOS 26): spelled as its value, so the iOS 17 build names no constant its SDK floor lacks
static constexpr NSUInteger kDataTypeDepthStencilState = 139;

// metal2vulkan's type_size_align: every object 8 bytes, a constant's MSL scalar, rows and columns
static bool argumentType(NSUInteger dataType, gm20b::ArgumentType &out)
{
    using K = gm20b::ArgumentKind;
    const auto handle = [&](K kind) {
        out = {kind, 0, 1, 1};
        return true;
    };
    const auto constant = [&](uint8_t scalar, uint8_t rows, uint8_t columns = 1) {
        out = {K::Constant, scalar, columns, rows};
        return true;
    };
    if (dataType == kDataTypeDepthStencilState)
        return handle(K::DepthStencilState);
    switch (MTLDataType(dataType)) {
    case MTLDataTypePointer:
        return handle(K::Buffer);
    case MTLDataTypeTexture:
        return handle(K::Texture);
    case MTLDataTypeSampler:
        return handle(K::Sampler);
    case MTLDataTypeIndirectCommandBuffer:
        return handle(K::IndirectCommandBuffer);
    case MTLDataTypeComputePipeline:
        return handle(K::ComputePipeline);
    case MTLDataTypeRenderPipeline:
        return handle(K::RenderPipeline);
    case MTLDataTypeVisibleFunctionTable:
        return handle(K::VisibleFunctionTable);
    case MTLDataTypeIntersectionFunctionTable:
        return handle(K::IntersectionFunctionTable);
    case MTLDataTypePrimitiveAccelerationStructure:
    case MTLDataTypeInstanceAccelerationStructure:
        return handle(K::AccelerationStructure);
    case MTLDataTypeFloat:
        return constant(4, 1);
    case MTLDataTypeFloat2:
        return constant(4, 2);
    case MTLDataTypeFloat3:
        return constant(4, 3);
    case MTLDataTypeFloat4:
        return constant(4, 4);
    case MTLDataTypeFloat2x2:
        return constant(4, 2, 2);
    case MTLDataTypeFloat2x3:
        return constant(4, 3, 2);
    case MTLDataTypeFloat2x4:
        return constant(4, 4, 2);
    case MTLDataTypeFloat3x2:
        return constant(4, 2, 3);
    case MTLDataTypeFloat3x3:
        return constant(4, 3, 3);
    case MTLDataTypeFloat3x4:
        return constant(4, 4, 3);
    case MTLDataTypeFloat4x2:
        return constant(4, 2, 4);
    case MTLDataTypeFloat4x3:
        return constant(4, 3, 4);
    case MTLDataTypeFloat4x4:
        return constant(4, 4, 4);
    case MTLDataTypeHalf:
        return constant(2, 1);
    case MTLDataTypeHalf2:
        return constant(2, 2);
    case MTLDataTypeHalf3:
        return constant(2, 3);
    case MTLDataTypeHalf4:
        return constant(2, 4);
    case MTLDataTypeHalf2x2:
        return constant(2, 2, 2);
    case MTLDataTypeHalf2x3:
        return constant(2, 3, 2);
    case MTLDataTypeHalf2x4:
        return constant(2, 4, 2);
    case MTLDataTypeHalf3x2:
        return constant(2, 2, 3);
    case MTLDataTypeHalf3x3:
        return constant(2, 3, 3);
    case MTLDataTypeHalf3x4:
        return constant(2, 4, 3);
    case MTLDataTypeHalf4x2:
        return constant(2, 2, 4);
    case MTLDataTypeHalf4x3:
        return constant(2, 3, 4);
    case MTLDataTypeHalf4x4:
        return constant(2, 4, 4);
    case MTLDataTypeBFloat:
        return constant(2, 1);
    case MTLDataTypeBFloat2:
        return constant(2, 2);
    case MTLDataTypeBFloat3:
        return constant(2, 3);
    case MTLDataTypeBFloat4:
        return constant(2, 4);
    case MTLDataTypeInt:
    case MTLDataTypeUInt:
        return constant(4, 1);
    case MTLDataTypeInt2:
    case MTLDataTypeUInt2:
        return constant(4, 2);
    case MTLDataTypeInt3:
    case MTLDataTypeUInt3:
        return constant(4, 3);
    case MTLDataTypeInt4:
    case MTLDataTypeUInt4:
        return constant(4, 4);
    case MTLDataTypeShort:
    case MTLDataTypeUShort:
        return constant(2, 1);
    case MTLDataTypeShort2:
    case MTLDataTypeUShort2:
        return constant(2, 2);
    case MTLDataTypeShort3:
    case MTLDataTypeUShort3:
        return constant(2, 3);
    case MTLDataTypeShort4:
    case MTLDataTypeUShort4:
        return constant(2, 4);
    case MTLDataTypeChar:
    case MTLDataTypeUChar:
    case MTLDataTypeBool:
        return constant(1, 1);
    case MTLDataTypeChar2:
    case MTLDataTypeUChar2:
    case MTLDataTypeBool2:
        return constant(1, 2);
    case MTLDataTypeChar3:
    case MTLDataTypeUChar3:
    case MTLDataTypeBool3:
        return constant(1, 3);
    case MTLDataTypeChar4:
    case MTLDataTypeUChar4:
    case MTLDataTypeBool4:
        return constant(1, 4);
    case MTLDataTypeLong:
    case MTLDataTypeULong:
        return constant(8, 1);
    case MTLDataTypeLong2:
    case MTLDataTypeULong2:
        return constant(8, 2);
    case MTLDataTypeLong3:
    case MTLDataTypeULong3:
        return constant(8, 3);
    case MTLDataTypeLong4:
    case MTLDataTypeULong4:
        return constant(8, 4);
    default:
        return false;
    }
}

static uint64_t resourceWord(id object)
{
    if (!object || ![object respondsToSelector:@selector(gpuResourceID)])
        return 0;
    const MTLResourceID resource = [(id<MTLTexture>)object gpuResourceID];
    uint64_t word;
    static_assert(sizeof resource == sizeof word);
    memcpy(&word, &resource, sizeof word);
    return word;
}

static void refuse(const char *call, NSString *why)
{
    once([NSString stringWithFormat:@"argument encoder %s refused: %@", call, why]);
}

static bool refused(const char *call, gm20b::ArgumentRefusal why, NSUInteger index = 0)
{
    using R = gm20b::ArgumentRefusal;
    switch (why) {
    case R::None:
        return false;
    case R::NotWritable:
        refuse(call, @"an argument buffer the CPU cannot write");
        break;
    case R::PastTheEnd:
        refuse(call, @"the encoded length passes the argument buffer's end");
        break;
    case R::NoBuffer:
        refuse(call, @"no argument buffer is set");
        break;
    case R::OtherType:
        refuse(call, [NSString stringWithFormat:@"index %lu holds an argument of another type", (unsigned long)index]);
        break;
    case R::NoIndex:
        refuse(call, [NSString stringWithFormat:@"no argument has index %lu", (unsigned long)index]);
        break;
    case R::OffsetPastEnd:
        refuse(call, @"an offset at or past the buffer's end");
        break;
    }
    return true;
}

@interface SwitchMetalArgumentEncoder : SwitchMetalObject <MTLArgumentEncoder> {
@public
    gm20b::ArgumentEncoder _encoder;
    id<MTLBuffer> _buffer;  // what _encoder writes into, kept alive while it does
}
@end

@implementation SwitchMetalArgumentEncoder
@dynamic device;  // SwitchMetalObject's

- (NSUInteger)encodedLength
{
    return NSUInteger(_encoder.layout().encodedLength);
}

- (NSUInteger)alignment
{
    return NSUInteger(_encoder.layout().alignment);
}

- (void)setArgumentBuffer:(id<MTLBuffer>)buffer startOffset:(NSUInteger)start arrayElement:(NSUInteger)element
{
    _buffer = nil;
    _encoder.clearBuffer();
    if (!buffer)
        return;
    if (![(id)buffer isKindOfClass:[SwitchMetalBuffer class]])
        return refuse("setArgumentBuffer", @"an argument buffer of another device");
    uint8_t *cpu = buffer.storageMode == MTLStorageModePrivate ? nullptr : static_cast<uint8_t *>(buffer.contents);
    if (!refused("setArgumentBuffer", _encoder.setBuffer(cpu, buffer.length, start, element)))
        _buffer = buffer;
}

- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset
{
    [self setArgumentBuffer:buffer startOffset:offset arrayElement:0];
}

- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index
{
    refused("setBuffer",
            _encoder.writeBuffer(buffer ? buffer.gpuAddress : 0, buffer ? buffer.length : 0, offset, index), index);
}

- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained[])buffers
           offsets:(const NSUInteger[])offsets
         withRange:(NSRange)range
{
    if (refused("setBuffers", _encoder.fits(range.location, range.length, gm20b::ArgumentKind::Buffer), range.location))
        return;
    for (NSUInteger i = 0; i < range.length; i++)
        if (buffers[i] && offsets[i] >= buffers[i].length) {
            refused("setBuffers", gm20b::ArgumentRefusal::OffsetPastEnd, range.location + i);
            return;
        }
    for (NSUInteger i = 0; i < range.length; i++)
        [self setBuffer:buffers[i] offset:offsets[i] atIndex:range.location + i];
}

- (void)object:(id)object index:(NSUInteger)index kind:(gm20b::ArgumentKind)kind call:(const char *)call
{
    refused(call, _encoder.write(resourceWord(object), index, kind), index);
}

- (void)objects:(const id __unsafe_unretained[])objects
          range:(NSRange)range
           kind:(gm20b::ArgumentKind)kind
           call:(const char *)call
{
    if (refused(call, _encoder.fits(range.location, range.length, kind), range.location))
        return;
    for (NSUInteger i = 0; i < range.length; i++)
        [self object:objects[i] index:range.location + i kind:kind call:call];
}

- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index
{
    [self object:texture index:index kind:gm20b::ArgumentKind::Texture call:"setTexture"];
}

- (void)setTextures:(const id<MTLTexture> __unsafe_unretained[])textures withRange:(NSRange)range
{
    [self objects:textures range:range kind:gm20b::ArgumentKind::Texture call:"setTextures"];
}

- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index
{
    [self object:sampler index:index kind:gm20b::ArgumentKind::Sampler call:"setSamplerState"];
}

- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained[])samplers withRange:(NSRange)range
{
    [self objects:samplers range:range kind:gm20b::ArgumentKind::Sampler call:"setSamplerStates"];
}

- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline atIndex:(NSUInteger)index
{
    [self object:pipeline index:index kind:gm20b::ArgumentKind::RenderPipeline call:"setRenderPipelineState"];
}

- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained[])pipelines
                      withRange:(NSRange)range
{
    [self objects:pipelines range:range kind:gm20b::ArgumentKind::RenderPipeline call:"setRenderPipelineStates"];
}

- (void)setComputePipelineState:(id<MTLComputePipelineState>)pipeline atIndex:(NSUInteger)index
{
    [self object:pipeline index:index kind:gm20b::ArgumentKind::ComputePipeline call:"setComputePipelineState"];
}

- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained[])pipelines
                       withRange:(NSRange)range
{
    [self objects:pipelines range:range kind:gm20b::ArgumentKind::ComputePipeline call:"setComputePipelineStates"];
}

- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)buffer atIndex:(NSUInteger)index
{
    [self object:buffer index:index kind:gm20b::ArgumentKind::IndirectCommandBuffer call:"setIndirectCommandBuffer"];
}

- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained[])buffers
                        withRange:(NSRange)range
{
    [self objects:buffers range:range kind:gm20b::ArgumentKind::IndirectCommandBuffer call:"setIndirectCommandBuffers"];
}

- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)structure atIndex:(NSUInteger)index
{
    [self object:structure index:index kind:gm20b::ArgumentKind::AccelerationStructure call:"setAccelerationStructure"];
}

- (void)setVisibleFunctionTable:(id<MTLVisibleFunctionTable>)table atIndex:(NSUInteger)index
{
    [self object:table index:index kind:gm20b::ArgumentKind::VisibleFunctionTable call:"setVisibleFunctionTable"];
}

- (void)setVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained[])tables
                       withRange:(NSRange)range
{
    [self objects:tables range:range kind:gm20b::ArgumentKind::VisibleFunctionTable call:"setVisibleFunctionTables"];
}

- (void)setIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)table atIndex:(NSUInteger)index
{
    [self object:table
           index:index
            kind:gm20b::ArgumentKind::IntersectionFunctionTable
            call:"setIntersectionFunctionTable"];
}

- (void)setIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained[])tables
                            withRange:(NSRange)range
{
    [self objects:tables
            range:range
             kind:gm20b::ArgumentKind::IntersectionFunctionTable
             call:"setIntersectionFunctionTables"];
}

- (void)setDepthStencilState:(id<MTLDepthStencilState>)state atIndex:(NSUInteger)index
{
    [self object:state index:index kind:gm20b::ArgumentKind::DepthStencilState call:"setDepthStencilState"];
}

- (void)setDepthStencilStates:(const id<MTLDepthStencilState> __unsafe_unretained[])states withRange:(NSRange)range
{
    [self objects:states range:range kind:gm20b::ArgumentKind::DepthStencilState call:"setDepthStencilStates"];
}

- (void *)constantDataAtIndex:(NSUInteger)index
{
    gm20b::ArgumentRefusal why;
    void *at = _encoder.constantData(index, why);
    refused("constantDataAtIndex", why, index);
    return at;
}

- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index
{
    gm20b::ArgumentEncoder inner;
    if (!_encoder.nested(index, inner))
        return nil;
    SwitchMetalArgumentEncoder *nested = [SwitchMetalArgumentEncoder new];
    nested->_encoder = std::move(inner);
    nested->_device = _device;
    return nested;
}

@end

@implementation SwitchMetalDevice (ArgumentEncoders)

- (id<MTLArgumentEncoder>)newArgumentEncoderWithArguments:(NSArray<MTLArgumentDescriptor *> *)arguments
{
    std::vector<gm20b::ArgumentDescription> descriptions;
    descriptions.reserve(arguments.count);
    for (MTLArgumentDescriptor *a in arguments) {
        gm20b::ArgumentDescription d;
        if (!argumentType(NSUInteger(a.dataType), d.type)) {
            refuse("creation", [NSString stringWithFormat:@"no argument has data type %lu", (unsigned long)a.dataType]);
            return nil;
        }
        d.index = uint32_t(a.index);
        d.arrayLength = uint32_t(a.arrayLength);
        d.constantBlockAlignment = uint32_t(a.constantBlockAlignment);
        descriptions.push_back(d);
    }
    gm20b::ArgumentLayout layout;
    size_t position = 0;
    if (gm20b::ArgumentLayout::fromDescriptions(descriptions, layout, position) != gm20b::ArgumentLayoutError::None) {
        refuse("creation", [NSString stringWithFormat:@"descriptor %zu is one Metal refuses", position]);
        return nil;
    }
    SwitchMetalArgumentEncoder *encoder = [SwitchMetalArgumentEncoder new];
    encoder->_encoder = gm20b::ArgumentEncoder(std::move(layout));
    encoder->_device = self;
    return encoder;
}

@end

static bool encoderOf(const gm20b::BufferEncoder &e, gm20b::ArgumentEncoder &out)
{
    std::vector<gm20b::ArgumentRuns> runs;
    for (const gm20b::FlatArgumentLayout &l : e.layouts)
        runs.push_back({l.encodedLength, l.alignment, l.runs.data(), uint32_t(l.runs.size())});
    return gm20b::ArgumentEncoder::fromRuns(runs.data(), uint32_t(runs.size()), out);
}

id<MTLArgumentEncoder> gm20bFunctionArgumentEncoder(SwitchMetalFunction *function, NSUInteger index)
{
    gm20b::BufferEncoders encoders;
    gm20b::ArgumentEncoder made;
    std::string why;
    if (gm20bBufferEncoders(function, encoders, why)) {
        const gm20b::BufferEncoder *e =
            index < gm20b::kBufferEncoderLimit ? gm20b::bufferEncoderAt(encoders, uint32_t(index)) : nullptr;
        if (!e)
            why = "the function declares no buffer there";
        else if (!e->refusal.empty())
            why = e->refusal;
        else if (!encoderOf(*e, made))
            why = "a layout of no argument kind";
    }
    if (!why.empty()) {
        refuse("creation",
               [NSString stringWithFormat:@"buffer %lu of %@: %s", (unsigned long)index, function.name, why.c_str()]);
        return nil;
    }
    SwitchMetalArgumentEncoder *encoder = [SwitchMetalArgumentEncoder new];
    encoder->_encoder = std::move(made);
    encoder->_device = function->_device;
    return encoder;
}
