// The registry for kits/SwitchDT on the host: entries whose properties are OSData bytes or OSStrings.
#ifndef SWITCH_TEST_SWITCH_DT_STUBS_H
#define SWITCH_TEST_SWITCH_DT_STUBS_H

#include <map>
#include <memory>
#include <string>
#include <vector>

class OSObject {
public:
    OSObject() = default;
    OSObject(const OSObject &) = delete;
    OSObject &operator=(const OSObject &) = delete;
    OSObject(OSObject &&) = delete;
    OSObject &operator=(OSObject &&) = delete;
    virtual ~OSObject() = default;
};

class OSData : public OSObject {
public:
    explicit OSData(std::vector<unsigned char> bytes) : bytes_(std::move(bytes))
    {}

    unsigned getLength() const
    {
        return static_cast<unsigned>(bytes_.size());
    }

    const void *getBytesNoCopy() const
    {
        return bytes_.data();
    }

private:
    std::vector<unsigned char> bytes_;
};

class OSString : public OSObject {};

#define OSDynamicCast(type, inst) dynamic_cast<type *>(const_cast<OSObject *>(static_cast<const OSObject *>(inst)))

class IORegistryEntry : public OSObject {
public:
    OSObject *getProperty(const char *key) const
    {
        const auto found = properties_.find(key);
        return found == properties_.end() ? nullptr : found->second.get();
    }

    void setBytes(const char *key, std::vector<unsigned char> bytes)
    {
        properties_[key] = std::make_unique<OSData>(std::move(bytes));
    }

    void setString(const char *key)
    {
        properties_[key] = std::make_unique<OSString>();
    }

private:
    std::map<std::string, std::unique_ptr<OSObject>> properties_;
};

#endif
