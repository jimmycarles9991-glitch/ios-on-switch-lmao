#ifndef SWITCH_DP_PROPERTIES_STUBS_H
#define SWITCH_DP_PROPERTIES_STUBS_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class OSObject {
public:
    virtual ~OSObject() = default;

    void retain() const
    {
        ++refs;
    }

    void release() const
    {
        if (--refs == 0)
            delete this;
    }

private:
    mutable unsigned refs = 1;
};

class OSString : public OSObject {
public:
    static OSString *withCString(const char *value)
    {
        return new OSString(value);
    }

    const char *getCStringNoCopy() const
    {
        return value.c_str();
    }

private:
    explicit OSString(const char *value) : value(value)
    {}

    std::string value;
};

class OSNumber : public OSObject {
public:
    static OSNumber *withNumber(uint64_t value, unsigned bits)
    {
        return new OSNumber(value, bits);
    }

    uint64_t unsigned64BitValue() const
    {
        return value;
    }

    unsigned numberOfBits() const
    {
        return bits;
    }

private:
    OSNumber(uint64_t value, unsigned bits) : value(value), bits(bits)
    {}

    uint64_t value;
    unsigned bits;
};

class OSBoolean : public OSObject {
public:
    explicit OSBoolean(bool value) : value(value)
    {}

    bool getValue() const
    {
        return value;
    }

private:
    bool value;
};

inline OSBoolean *const kOSBooleanTrue = new OSBoolean(true);
inline OSBoolean *const kOSBooleanFalse = new OSBoolean(false);

class OSData : public OSObject {
public:
    static OSData *withBytes(const void *bytes, unsigned length)
    {
        return new OSData(bytes, length);
    }

    unsigned getLength() const
    {
        return static_cast<unsigned>(bytes.size());
    }

    const void *getBytesNoCopy() const
    {
        return bytes.data();
    }

private:
    OSData(const void *source, unsigned length)
        : bytes(static_cast<const uint8_t *>(source), static_cast<const uint8_t *>(source) + length)
    {}

    std::vector<uint8_t> bytes;
};

class OSArray : public OSObject {
public:
    static OSArray *withCapacity(unsigned)
    {
        return new OSArray;
    }

    ~OSArray() override
    {
        for (auto *object : objects)
            object->release();
    }

    bool setObject(const OSObject *object)
    {
        object->retain();
        objects.push_back(const_cast<OSObject *>(object));
        return true;
    }

    OSObject *getObject(unsigned index) const
    {
        return objects.at(index);
    }

    unsigned getCount() const
    {
        return static_cast<unsigned>(objects.size());
    }

private:
    std::vector<OSObject *> objects;
};

class OSDictionary : public OSObject {
public:
    static OSDictionary *withCapacity(unsigned)
    {
        return new OSDictionary;
    }

    ~OSDictionary() override
    {
        for (const auto &entry : objects)
            entry.second->release();
    }

    bool setObject(const char *key, const OSObject *object)
    {
        object->retain();
        auto &existing = objects[key];
        if (existing)
            existing->release();
        existing = const_cast<OSObject *>(object);
        return true;
    }

    OSObject *getObject(const char *key) const
    {
        const auto found = objects.find(key);
        return found == objects.end() ? nullptr : found->second;
    }

    unsigned getCount() const
    {
        return static_cast<unsigned>(objects.size());
    }

private:
    std::map<std::string, OSObject *> objects;
};

#define OSDynamicCast(type, object) dynamic_cast<type *>(object)
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object) {             \
            (object)->release();  \
            (object) = nullptr;   \
        }                         \
    } while (0)

#endif
