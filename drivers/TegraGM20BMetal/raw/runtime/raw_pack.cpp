// SPDX-License-Identifier: MIT
// See raw_pack.h.
#include "raw_pack.h"

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace raw {

uint64_t Pack::fnv(const void *p, size_t n)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

Pack::~Pack()
{
    if (base_)
        munmap(const_cast<uint8_t *>(base_), size_);
}

bool Pack::load(const char *path, uint64_t stack, std::string &err)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        err = std::string("cannot open ") + path;
        return false;
    }
    struct stat st;
    fstat(fd, &st);
    void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (m == MAP_FAILED) {
        err = "mmap failed";
        return false;
    }
    base_ = static_cast<const uint8_t *>(m);
    size_ = st.st_size;
    hdr_ = reinterpret_cast<const PackHeader *>(base_);
    if (size_ < sizeof(PackHeader) || memcmp(hdr_->magic, "GM20BPK1", 8) || hdr_->version != 4) {
        err = "not a version-4 pack";
        return false;
    }
    if (stack && hdr_->stack != stack) {
        err = "the pack is another build's (stack key differs)";
        return false;
    }
    for (int s = 0; s < PackSectionCount; s++)
        if (hdr_->section[s].offset + hdr_->section[s].bytes > size_) {
            err = "truncated pack";
            return false;
        }
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    for (uint32_t i = 0; i < hdr_->pipelines; i++)
        index_.push_back({p[i].keyHash, i});
    std::sort(index_.begin(), index_.end());
    return true;
}

const PackPipeline *Pack::find(uint64_t keyHash, PackForm form) const
{
    const PackPipeline *p = section<PackPipeline>(PackPipelines);
    for (auto it = std::lower_bound(index_.begin(), index_.end(), std::make_pair(keyHash, 0u));
         it != index_.end() && it->first == keyHash; ++it)
        if (form == PackForm::Default || (p[it->second].flags & PipelineBarrierForm))
            return p + it->second;
    return nullptr;
}

}  // namespace raw
