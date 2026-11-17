// SPDX-License-Identifier: BSD-2-Clause-Patent
#import "EntitlementDER.h"

#include <CoreEntitlements/CoreEntitlements.h>
#include <dlfcn.h>
#include <vector>

extern "C" CEError_t CESizeDeserialization(CEQueryContext_t, size_t *);
extern "C" CEError_t CEDeserialize(CEQueryContext_t, CESerializedElement_t *, size_t);

static void *switch_core_entitlements(void)
{
    static void *library = dlopen("/usr/lib/libCoreEntitlements.dylib", RTLD_LAZY);
    return library;
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
    void *library = switch_core_entitlements();
    auto size = library ? (decltype(&CESizeSerialization))dlsym(library, "CESizeSerialization") : nullptr;
    auto serialize = library ? (decltype(&CESerialize))dlsym(library, "CESerialize") : nullptr;
    auto success = library ? (CEError_t *)dlsym(library, "kCENoError") : nullptr;
    if (!size || !serialize || !success)
        return nil;
    std::vector<CESerializedElement_t> elements;
    if (!switch_append_elements(dictionary, elements))
        return nil;
    size_t length = 0;
    if (size(elements.data(), elements.size(), &length) != *success || !length || length > 65536)
        return nil;
    NSMutableData *data = [NSMutableData dataWithLength:length];
    uint8_t *bytes = (uint8_t *)data.mutableBytes;
    if (serialize(&switch_entitlement_runtime, elements.data(), elements.size(), bytes, bytes + length) != *success)
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
    void *library = switch_core_entitlements();
    auto validate = library ? (decltype(&CEValidate))dlsym(library, "CEValidate") : nullptr;
    auto acquire = library ? (decltype(&CEAcquireManagedContext))dlsym(library, "CEAcquireManagedContext") : nullptr;
    auto release = library ? (decltype(&CEReleaseManagedContext))dlsym(library, "CEReleaseManagedContext") : nullptr;
    auto size = library ? (decltype(&CESizeDeserialization))dlsym(library, "CESizeDeserialization") : nullptr;
    auto deserialize = library ? (decltype(&CEDeserialize))dlsym(library, "CEDeserialize") : nullptr;
    auto success = library ? (CEError_t *)dlsym(library, "kCENoError") : nullptr;
    if (!data.length || data.length > 65536 || !validate || !acquire || !release || !size || !deserialize || !success)
        return nil;
    const uint8_t *bytes = (const uint8_t *)data.bytes;
    CEValidationResult validation = {};
    if (validate(&switch_entitlement_runtime, &validation, bytes, bytes + data.length) != *success)
        return nil;
    CEQueryContext_t context = nullptr;
    if (acquire(&switch_entitlement_runtime, validation, &context) != *success)
        return nil;
    size_t count = 0;
    NSDictionary *result = nil;
    if (size(context, &count) == *success && count && count < 65536) {
        std::vector<CESerializedElement_t> elements(count);
        if (deserialize(context, elements.data(), count) == *success) {
            size_t position = 0;
            id value = switch_read_element(elements, position);
            if ([value isKindOfClass:NSDictionary.class] && position == count)
                result = value;
        }
    }
    (void)release(&context);
    return result;
}
