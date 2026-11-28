// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "EntitlementDER.h"

#include <CoreEntitlements/CoreEntitlements.h>
#include <dlfcn.h>
#include <vector>

extern "C" CEError_t CESizeDeserialization(CEQueryContext_t, size_t *);
extern "C" CEError_t CEDeserialize(CEQueryContext_t, CESerializedElement_t *, size_t);

struct SwitchCoreEntitlements {
    decltype(&CESizeSerialization) sizeSerialization;
    decltype(&CESerialize) serialize;
    decltype(&CEValidate) validate;
    decltype(&CEAcquireManagedContext) acquire;
    decltype(&CEReleaseManagedContext) release;
    decltype(&CESizeDeserialization) sizeDeserialization;
    decltype(&CEDeserialize) deserialize;
    CEError_t *noError;
};

static const SwitchCoreEntitlements *switch_core_entitlements(void)
{
    static SwitchCoreEntitlements symbols;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        void *library = dlopen("/usr/lib/libCoreEntitlements.dylib", RTLD_LAZY);
        if (!library)
            return;
        symbols.sizeSerialization = (decltype(&CESizeSerialization))dlsym(library, "CESizeSerialization");
        symbols.serialize = (decltype(&CESerialize))dlsym(library, "CESerialize");
        symbols.validate = (decltype(&CEValidate))dlsym(library, "CEValidate");
        symbols.acquire = (decltype(&CEAcquireManagedContext))dlsym(library, "CEAcquireManagedContext");
        symbols.release = (decltype(&CEReleaseManagedContext))dlsym(library, "CEReleaseManagedContext");
        symbols.sizeDeserialization = (decltype(&CESizeDeserialization))dlsym(library, "CESizeDeserialization");
        symbols.deserialize = (decltype(&CEDeserialize))dlsym(library, "CEDeserialize");
        symbols.noError = (CEError_t *)dlsym(library, "kCENoError");
    });
    return &symbols;
}

static void __attribute__((noreturn)) switch_entitlement_abort(CERuntime_t, const char *, ...)
{
    abort();
}

static const CERuntime switch_entitlement_runtime = {
    CE_RUNTIME_VERSION,
    +[](CERuntime_t, size_t size) -> void * { return malloc(size); },
    +[](CERuntime_t, void *memory) { free(memory); },
    nullptr,
    switch_entitlement_abort,
    nullptr,
    nullptr,
    nullptr,
};

static BOOL switch_append_elements(id object, std::vector<CESerializedElement_t> &elements)
{
    CESerializedElement_t element = {};
    if ([object isKindOfClass:NSDictionary.class]) {
        element.type = kCESerializedDictionaryBegin;
        elements.push_back(element);
        for (NSString *key in [[object allKeys] sortedArrayUsingSelector:@selector(compare:)]) {
            element = {};
            element.type = kCESerializedArrayBegin;
            elements.push_back(element);
            element.type = kCESerializedKey;
            CESerializedElementSetData(&element, (void *)key.UTF8String, [key lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
            elements.push_back(element);
            if (!switch_append_elements(object[key], elements))
                return NO;
            element = {};
            element.type = kCESerializedArrayEnd;
            elements.push_back(element);
        }
        element.type = kCESerializedDictionaryEnd;
    } else if ([object isKindOfClass:NSArray.class]) {
        element.type = kCESerializedArrayBegin;
        elements.push_back(element);
        for (id value in object) {
            if (!switch_append_elements(value, elements))
                return NO;
        }
        element.type = kCESerializedArrayEnd;
    } else if ([object isKindOfClass:NSString.class]) {
        element.type = kCESerializedString;
        CESerializedElementSetData(&element, (void *)[object UTF8String], [object lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
    } else if ([object isKindOfClass:NSData.class]) {
        element.type = kCESerializedData;
        CESerializedElementSetData(&element, (void *)[object bytes], [object length]);
    } else if ([object isKindOfClass:NSNumber.class]) {
        element.type = CFGetTypeID((__bridge CFTypeRef)object) == CFBooleanGetTypeID() ? kCESerializedBool : kCESerializedInteger;
        element.data.value = [object longLongValue];
    } else {
        return NO;
    }
    elements.push_back(element);
    return YES;
}

NSData *switch_serialize_entitlements(NSDictionary *dictionary)
{
    const SwitchCoreEntitlements *ce = switch_core_entitlements();
    if (!ce->sizeSerialization || !ce->serialize || !ce->noError)
        return nil;
    std::vector<CESerializedElement_t> elements;
    if (!switch_append_elements(dictionary, elements))
        return nil;
    size_t length = 0;
    if (ce->sizeSerialization(elements.data(), elements.size(), &length) != *ce->noError || !length || length > 65536)
        return nil;
    NSMutableData *data = [NSMutableData dataWithLength:length];
    uint8_t *bytes = (uint8_t *)data.mutableBytes;
    if (ce->serialize(&switch_entitlement_runtime, elements.data(), elements.size(), bytes, bytes + length) !=
        *ce->noError)
        return nil;
    return [switch_deserialize_entitlements(data) isEqual:dictionary] ? data : nil;
}

static id switch_read_element(const std::vector<CESerializedElement_t> &elements, size_t &position)
{
    if (position >= elements.size())
        return nil;
    const auto &element = elements[position++];
    switch (element.type) {
    case kCESerializedBool:
        return @(!!element.data.value);
    case kCESerializedInteger:
        return @(element.data.value);
    case kCESerializedString:
    case kCESerializedKey:
        return [[NSString alloc] initWithBytes:CESerializedElementGetData(&element) length:element.length encoding:NSUTF8StringEncoding];
    case kCESerializedData:
        return [NSData dataWithBytes:CESerializedElementGetData(&element) length:element.length];
    case kCESerializedArrayBegin: {
        NSMutableArray *array = [NSMutableArray array];
        while (position < elements.size() && elements[position].type != kCESerializedArrayEnd) {
            id value = switch_read_element(elements, position);
            if (!value)
                return nil;
            [array addObject:value];
        }
        if (position == elements.size())
            return nil;
        ++position;
        return array;
    }
    case kCESerializedDictionaryBegin: {
        NSMutableDictionary *dictionary = [NSMutableDictionary dictionary];
        while (position < elements.size() && elements[position].type != kCESerializedDictionaryEnd) {
            id pair = switch_read_element(elements, position);
            if (![pair isKindOfClass:NSArray.class] || [pair count] != 2 || ![pair[0] isKindOfClass:NSString.class])
                return nil;
            dictionary[pair[0]] = pair[1];
        }
        if (position == elements.size())
            return nil;
        ++position;
        return dictionary;
    }
    default:
        return nil;
    }
}

NSDictionary *switch_deserialize_entitlements(NSData *data)
{
    const SwitchCoreEntitlements *ce = switch_core_entitlements();
    if (!data.length || data.length > 65536 || !ce->validate || !ce->acquire || !ce->release ||
        !ce->sizeDeserialization || !ce->deserialize || !ce->noError)
        return nil;
    const uint8_t *bytes = (const uint8_t *)data.bytes;
    CEValidationResult validation = {};
    if (ce->validate(&switch_entitlement_runtime, &validation, bytes, bytes + data.length) != *ce->noError)
        return nil;
    CEQueryContext_t context = nullptr;
    if (ce->acquire(&switch_entitlement_runtime, validation, &context) != *ce->noError)
        return nil;
    size_t count = 0;
    NSDictionary *result = nil;
    if (ce->sizeDeserialization(context, &count) == *ce->noError && count && count < 65536) {
        std::vector<CESerializedElement_t> elements(count);
        if (ce->deserialize(context, elements.data(), count) == *ce->noError) {
            size_t position = 0;
            id value = switch_read_element(elements, position);
            if ([value isKindOfClass:NSDictionary.class] && position == count)
                result = value;
        }
    }
    (void)ce->release(&context);
    return result;
}
