// SPDX-License-Identifier: MIT
// See compiler_store.h.
#include "compiler_store.h"
#include "../raw/runtime/raw_compute_slot.h"
#include "../raw/runtime/raw_program_pages.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <vector>

namespace switch_compiler {
namespace {

constexpr char kPrefix[] = "raw-";

void identityOf(uint64_t stack, const raw::Digest &rules, uint8_t identity[switch_seed_pack::kIdentityBytes])
{
    static_assert(switch_seed_pack::kIdentityBytes == 32, "the stack and 24 bytes of the rules' digest");
    memcpy(identity, &stack, sizeof stack);
    memcpy(identity + sizeof stack, rules.data(), switch_seed_pack::kIdentityBytes - sizeof stack);
}

// Every store directory under `root` but `ours`, with what a store writer leaves there. A process still mapping one of
// their files keeps reading it: unlinking never cuts a file short.
void removeOthers(const std::string &root, const std::string &ours)
{
    DIR *d = opendir(root.c_str());
    if (!d)
        return;
    std::vector<std::string> stale;
    while (const dirent *e = readdir(d)) {
        const std::string name = e->d_name;
        if (!name.compare(0, sizeof kPrefix - 1, kPrefix) && name != ours &&
            name.find_first_not_of("0123456789abcdef-", sizeof kPrefix - 1) == std::string::npos)
            stale.push_back(root + "/" + name);
    }
    closedir(d);
    for (const std::string &dir : stale) {
        for (const char *file : {"/store", "/store.new", "/store.new.new"})
            unlink((dir + file).c_str());
        rmdir(dir.c_str());
    }
}

}  // namespace

std::string storeDirectory(uint64_t stack, const raw::Digest &rules)
{
    char name[48];
    uint64_t r = 0;
    memcpy(&r, rules.data(), sizeof r);
    snprintf(name, sizeof name, "%s%016llx-%016llx", kPrefix, static_cast<unsigned long long>(stack),
             static_cast<unsigned long long>(r));
    return name;
}

bool Store::open(const std::string &root, uint64_t stack, const raw::Digest &rules, std::string &error)
{
    const std::string ours = storeDirectory(stack, rules), dir = root + "/" + ours;
    if ((mkdir(root.c_str(), 0755) && errno != EEXIST) || (mkdir(dir.c_str(), 0755) && errno != EEXIST))
        return error = "cannot make " + dir + ": " + strerror(errno), false;
    removeOthers(root, ours);
    identityOf(stack, rules, identity_);
    const uint8_t *identity = identity_;
    path_ = dir + "/store";
    std::lock_guard<std::mutex> l(lock_);
    if (!writer_.open(path_, identity, kSlots, kDataBytes, error))
        return false;
    // Retired entries are reclaimed here, before this compiler registers the file
    const bool crowded = writer_.retired() * 4 > kSlots || writer_.dataUsed() > kDataBytes / 4 * 3 ||
                         (writer_.used() + writer_.retired()) * 4 > kSlots * 3 - kSlots / 4;
    if (!crowded)
        return true;
    writer_.close();
    std::string compacted;
    if (!switch_seed_pack::compactStore(path_, identity, kSlots, kDataBytes, compacted))
        unlink(path_.c_str());
    return writer_.open(path_, identity, kSlots, kDataBytes, error);
}

bool Store::find(const raw::Digest &name, std::string &bytes) const
{
    std::lock_guard<std::mutex> l(lock_);
    switch_seed_pack::Bytes found;
    if (!writer_.reader().find(name.data(), found))
        return false;
    bytes.assign(reinterpret_cast<const char *>(found.data), found.size);
    return true;
}

void Store::retire(const raw::Digest &name)
{
    std::lock_guard<std::mutex> l(lock_);
    writer_.retire(name.data());
}

Store::Put Store::put(const raw::Digest &name, const std::string &entry)
{
    std::lock_guard<std::mutex> l(lock_);
    if (entry.size() > UINT32_MAX)
        return Put::Failed;
    switch (writer_.put(name.data(), reinterpret_cast<const uint8_t *>(entry.data()), uint32_t(entry.size()), true)) {
    case switch_seed_pack::StoreWriter::Put::Added:
        return Put::Added;
    case switch_seed_pack::StoreWriter::Put::Full:
        return Put::Full;
    default:
        return Put::Failed;
    }
}

// The slots a record's or a kernel's entry holds, as the client lays each out to place it (raw_completed.cpp, raw_compute.cpp)
static std::vector<std::vector<uint8_t>> entrySlots(const raw::CacheKey &key, const std::string &entry)
{
    std::vector<std::vector<uint8_t>> slots;
    std::string refusal;
    raw::RuntimeRecord record;
    if (raw::decodeEntry(entry, key, record, refusal) == raw::RecordCache::Found::Record) {
        for (const raw::CapturedProgram *program : raw::slotPrograms(record)) {
            raw::PackProgram info;
            if (!program->image.empty())
                slots.push_back(raw::programSlot(*program, info));
        }
        return slots;
    }
    raw::ComputeRecord kernel;
    if (raw::decodeKernelEntry(entry, key, kernel, refusal) == raw::RecordCache::Found::Record)
        slots.push_back(raw::computeSlot(kernel));
    return slots;
}

Store::Put Store::putProgramSlots(const raw::CacheKey &key, const std::string &entry)
{
    for (std::vector<uint8_t> &slot : entrySlots(key, entry)) {
        if (slot.size() < raw::kProgramSlotPage)
            continue;
        const raw::Digest name = raw::programSlotName(slot.data(), slot.size());
        slot.resize(raw::programSlotEntryBytes(slot.size()));
        std::lock_guard<std::mutex> l(lock_);
        if (writer_.put(name.data(), slot.data(), uint32_t(slot.size())) == switch_seed_pack::StoreWriter::Put::Full)
            return Put::Full;
    }
    return Put::Added;
}

bool Store::makeRoom(std::string &error)
{
    std::lock_guard<std::mutex> l(lock_);
    writer_.close();
    if (!switch_seed_pack::compactStore(path_, identity_, kSlots, kDataBytes, error, kDataBytes / 2, kSlots / 4)) {
        unlink(path_.c_str());
        error = "the full store could not be written again (" + error + "): made anew";
    }
    return writer_.open(path_, identity_, kSlots, kDataBytes, error);
}

}  // namespace switch_compiler
