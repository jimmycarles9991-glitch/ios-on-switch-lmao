// SPDX-License-Identifier: GPL-2.0-only
#include <CommonCrypto/CommonDigest.h>
#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
constexpr size_t SHA256_DIGEST_LENGTH = CC_SHA256_DIGEST_LENGTH;
using SHA256_CTX = CC_SHA256_CTX;
#define SHA256_Init CC_SHA256_Init
#define SHA256_Update CC_SHA256_Update
#define SHA256_Final CC_SHA256_Final

struct Vnode {
    std::vector<uint8_t> bytes;
    int mount = 1, references = 0, lookup_error = 0, read_error = 0, reads = 0;
    bool regular = true, root = false, supported = true, short_read = false;
};
using vnode_t = Vnode *;
using proc_t = int;
using DTEntry = int;
using vfs_context_t = int;
constexpr vnode_t NULLVP = nullptr;
constexpr int kSuccess = 0, UIO_READ = 0, UIO_SYSSPACE = 0, Z_WAITOK = 0;
struct vnode_attr { uint64_t va_data_size; bool supported; };
#define VATTR_INIT(va) (*(va) = {})
#define VATTR_WANTED(va, field) ((void)0)
#define VATTR_IS_SUPPORTED(va, field) ((va)->supported)

Vnode data, catalog, signature;
std::array<uint8_t, 2 * SHA256_DIGEST_LENGTH> digests;
unsigned property_size;
bool property_present;

int SecureDTGetProperty(DTEntry, const char *name, const void **value, unsigned *size)
{
    EXPECT_STREQ(name, "switch-launchd-catalog-sha256");
    if (!property_present) return ENOENT;
    *value = digests.data();
    *size = property_size;
    return kSuccess;
}

int switch_launchd_catalog_lookup(const char *path, vnode_t parent, vnode_t *out, vfs_context_t)
{
    Vnode *node = &data;
    if (parent) {
        EXPECT_EQ(parent, &data);
        constexpr char catalog_path[] = "Switch/overlay/System/Library/xpc/launchd.plist";
        node = std::strcmp(path, catalog_path) ? &signature : &catalog;
        if (node == &signature) EXPECT_STREQ(path, "Switch/overlay/System/Library/xpc/launchd.plist.sig");
    } else {
        EXPECT_STREQ(path, "/private/var");
    }
    if (node->lookup_error) return node->lookup_error;
    ++node->references;
    *out = node;
    return 0;
}

int vfs_context_current() { return 0; }
int vfs_context_ucred(vfs_context_t) { return 0; }
bool vnode_isreg(vnode_t node) { return node->regular; }
bool vnode_isvroot(vnode_t node) { return node->root; }
int vnode_mount(vnode_t node) { return node->mount; }
void vnode_put(vnode_t node) { --node->references; }
int vnode_getattr(vnode_t node, vnode_attr *attr, vfs_context_t) { *attr = {node->bytes.size(), node->supported}; return 0; }
char *kalloc_data(size_t capacity, int) { return static_cast<char *>(std::malloc(capacity)); }
void kfree_data(char *buffer, size_t) { std::free(buffer); }

int vn_rdwr(int, vnode_t node, char *out, int count, off_t offset, int, int, int, int *residual, proc_t)
{
    EXPECT_GT(count, 0);
    EXPECT_LE(count, 64 * 1024);
    EXPECT_LE(static_cast<size_t>(offset) + count, node->bytes.size());
    ++node->reads;
    if (node->read_error) return node->read_error;
    *residual = node->short_read ? 1 : 0;
    std::memcpy(out, node->bytes.data() + offset, count - *residual);
    return 0;
}

#include "launchd_catalog_guard.h"

class LaunchdCatalogGuard : public testing::Test {
protected:
    void SetUp() override
    {
        data = catalog = signature = {};
        data.root = true;
        catalog.bytes.assign(128 * 1024 + 1, 'c');
        signature.bytes.assign(21439, 's');
        CC_SHA256(catalog.bytes.data(), catalog.bytes.size(), digests.data());
        CC_SHA256(signature.bytes.data(), signature.bytes.size(), digests.data() + SHA256_DIGEST_LENGTH);
        property_present = true;
        property_size = digests.size();
    }
    void TearDown() override
    {
        EXPECT_EQ(data.references, 0);
        EXPECT_EQ(catalog.references, 0);
        EXPECT_EQ(signature.references, 0);
    }
};

TEST_F(LaunchdCatalogGuard, ReadsBothFilesInBoundedChunks)
{
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), 0);
    EXPECT_EQ(catalog.reads, 3);
    EXPECT_EQ(signature.reads, 1);
    signature.bytes.back() ^= 1;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EAUTH);
    signature.bytes.back() ^= 1;
    catalog.bytes.back() ^= 1;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EAUTH);
}

TEST_F(LaunchdCatalogGuard, RejectsMissingMalformedOrUnreadablePair)
{
    property_present = false;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EINVAL);
    property_present = true;
    --property_size;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EINVAL);
    ++property_size;
    signature.lookup_error = ENOENT;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), ENOENT);
    signature.lookup_error = 0;
    signature.regular = false;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EPERM);
    signature.regular = true;
    signature.mount = 2;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EPERM);
    signature.mount = 1;
    signature.short_read = true;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EIO);
    signature.short_read = false;
    signature.read_error = EIO;
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EIO);
    signature.read_error = 0;
    signature.bytes.resize(256 * 1024 + 1);
    EXPECT_EQ(switch_launchd_catalog_pair(0, 0), EFBIG);
}
}
