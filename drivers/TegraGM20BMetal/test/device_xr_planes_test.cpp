// SPDX-License-Identifier: MIT
// RGB10A8_2P_XR10 held in a surface's planes (createXrPlaneTexture): over two planes whose rows are off 32 bytes, its
// header pair reads the planes' pool images; over a plane alone, its alpha a memoryless target whose header, freed with the
// pair, is over a sparse range of its own (reads 0, stores dropped); of rows under 16 bytes, read and stored at Metal's
// two strides.
#include "device_views_harness.h"
#include "../xr_pass.h"
#include "../xr_pools.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace views;

constexpr uint32_t kRGBA8 = 70;

// A header's address to 512 bytes, pitch (ADDRESS_BITS31TO5) or block-linear (fx_mip.cpp ADDRESS_BITS31TO9)
uint64_t headerAddress(const uint32_t *w)
{
    return uint64_t(w[1] & ~511u) | uint64_t(w[2] & 0xffff) << 32;
}

struct Surface {
    size_t bytes = 8 * size_t(getpagesize());
    void *base = aligned_alloc(size_t(getpagesize()), bytes);

    ~Surface()
    {
        free(base);
    }
};

TEST_F(DeviceViews, PlanesOfRowsOff32AreReadThroughTheirPools)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    // 64x64: words in rows of 272 bytes, alpha bytes in rows of 80 after them
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 272, 64 * 272, 80, 64, 64, 550, t, &why))
        << why;
    ASSERT_TRUE(t.xrPlanes && t.luma->sampled && t.chroma->sampled);
    for (uint32_t k = 0; k < 2; k++) {
        const gm20b::Texture &pool = *(k ? t.chroma : t.luma)->sampled;
        EXPECT_EQ(headerAddress(dev.headerWords(t.tic + k)), pool.va & ~uint64_t(511)) << "plane " << k;
    }
    dev.destroyTexture(t);
}

// Words of rows 64 for 32 texels and alpha bytes of rows 16 for 32 (an M4 Max reads each at its plane's pitch, the rows
// overlapping, and its passes store each byte from the row overlap_order.h names): both planes read through pools, the
// header pair over them
TEST_F(DeviceViews, PlanesThatOverlapAreHeldThroughPools)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 64, 1024, 16, 32, 8, 550, t, &why)) << why;
    ASSERT_TRUE(t.xrPlanes && t.luma->sampled && t.chroma->sampled);
    EXPECT_EQ(t.luma->pitch, 64u) << "each row at its plane's pitch";
    EXPECT_EQ(t.chroma->pitch, 16u);
    EXPECT_TRUE(gm20b::xrPlanesOverlap(t));
    for (uint32_t k = 0; k < 2; k++) {
        const gm20b::Texture &pool = *(k ? t.chroma : t.luma)->sampled;
        EXPECT_EQ(headerAddress(dev.headerWords(t.tic + k)), pool.va & ~uint64_t(511)) << "plane " << k;
    }
    dev.destroyTexture(t);
}

class DeviceXrSink : public DeviceViews {
protected:
    std::vector<TegraGM20BAbiBindOp> ops;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.infoFlags = TegraGM20BInfoSparse;
        kernel_model::model.bound = [this](const TegraGM20BAbiBindOp &op) {
            ops.push_back(op);
        };
        std::string err;
        ASSERT_TRUE(dev.open(harness::emptyPack().c_str(), 0, err, gm20b::Device::Role::Client)) << err;
    }
};

// An M4 Max reads a plane alone's alpha as 0 and stores none of it (m2v-textures9 probes/xr.txt 6a-6c): its header is over
// a sparse range of the texture's own, one VmBind MapSparse at creation, unbound with the texture
TEST_F(DeviceXrSink, APlaneAlonesAlphaIsASparseSinkFreedWithItsPair)
{
    Surface s;
    gm20b::Texture t, other, plain;
    const char *why = "";
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, t, &why)) << why;
    ASSERT_TRUE(t.xrPlanes && t.luma && t.chroma);
    EXPECT_TRUE(t.chroma->memoryless);
    EXPECT_EQ(t.chroma->tic, t.tic + 1);
    const uint64_t va = t.window.va, bytes = t.window.bytes;
    ASSERT_GE(bytes, 64u * 64u);
    const auto sparse = [&](uint32_t op) {
        size_t n = 0;
        for (const TegraGM20BAbiBindOp &o : ops)
            n += o.op == op && o.va == va && o.range == bytes;
        return n;
    };
    EXPECT_EQ(sparse(TegraGM20BBindMapSparse), 1u);
    const uint32_t *alpha = dev.headerWords(t.tic + 1);
    EXPECT_EQ(uint64_t(alpha[1] & ~31u) | uint64_t(alpha[2] & 0xffff) << 32, va) << "a pitch header over it";
    dev.destroyTexture(t);
    EXPECT_EQ(sparse(TegraGM20BBindUnmap), 1u) << "unbound with the texture";
    ASSERT_TRUE(dev.createTexture(16, 16, kRGBA8, plain));
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, other, &why)) << why;
    EXPECT_NE(plain.tic, other.tic);
    EXPECT_NE(plain.tic, other.tic + 1) << "the alpha's header went back with the pair alone";
    dev.destroyTexture(other);
    dev.destroyTexture(plain);
}

TEST_F(DeviceViews, APlaneAlonesAlphaNeedsSparseVa)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    EXPECT_FALSE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, t, &why));
    EXPECT_NE(std::string(why).find("TegraGM20BInfoSparse"), std::string::npos) << why;
}

// A plane alone of rows that hold its words keeps no pool: only rows that overlap are read and stored through one
// A plane alone whose rows overlap and run on past the surface's last page: its words are read through a window of the
// surface's pages and a sparse tail (reads 0, as an M4 Max reads past a surface's last page where nothing else is mapped),
// and its alpha's header is over a sparse range at the window's end, past every word row
TEST_F(DeviceXrSink, APlaneAloneWhoseRowsRunPastTheLastPageReadsThroughAWindow)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    const uint64_t word = s.bytes - 256;
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, word, 64, 0, 0, 32, 8, 550, t, &why)) << why;
    ASSERT_TRUE(t.xrPlanes && t.luma->sampled && t.chroma->memoryless);
    ASSERT_GT(t.window.bytes, t.window.pages);
    EXPECT_GE(t.luma->va, t.window.va) << "the words read in the window";
    const uint64_t alpha = headerAddress(dev.headerWords(t.tic + 1));
    EXPECT_GE(alpha, t.luma->va + 64 * 7 + 4 * 32) << "the sink past every word row";
    EXPECT_LT(alpha, t.window.va + t.window.bytes);
    dev.destroyTexture(t);
}

// Planes of rows under 16 bytes, which an M4 Max reads 1 MiB apart and stores 16 apart, the words of rows 4-7 landing on
// the alpha plane (m2v-textures11 probes/h1/rows550.txt a1): each read through its pool from rows 1 MiB apart in a window
// of the surface's pages and a sparse tail, and stored from it at rows 16 apart, the two planes' stores crossing
TEST_F(DeviceXrSink, PlanesOfRowsUnderSixteenBytesAreReadAMebibyteApartAndStoredSixteenApart)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 8, 64, 2, 2, 8, 550, t, &why)) << why;
    ASSERT_TRUE(t.xrPlanes && t.luma->sampled && t.chroma->sampled);
    for (const gm20b::Texture *plane : {t.luma, t.chroma}) {
        EXPECT_EQ(plane->pitch, 1u << 20) << "read 1 MiB apart";
        EXPECT_EQ(plane->storePitch, 16u) << "stored 16 apart";
        EXPECT_EQ(gm20b::storedRows(*plane, t).pitch, 16u);
    }
    ASSERT_GT(t.window.bytes, t.window.pages) << "rows past the surface's pages through a sparse tail";
    EXPECT_EQ(t.chroma->va - t.luma->va, 64u);
    EXPECT_EQ(gm20b::storedRows(*t.luma, t).storage.bytes, 16u * 7 + 8);
    EXPECT_TRUE(gm20b::xrPlanesCross(gm20b::storedRows(*t.luma, t), gm20b::storedRows(*t.chroma, t)));
    EXPECT_TRUE(gm20b::xrPlanesOverlap(t));
    for (uint32_t k = 0; k < 2; k++)
        EXPECT_EQ(headerAddress(dev.headerWords(t.tic + k)), (k ? t.chroma : t.luma)->sampled->va & ~uint64_t(511));
    dev.destroyTexture(t);

    // words of rows 32 and alpha bytes of rows 8 (rows550.txt m3): the alpha alone at two strides
    gm20b::Texture u;
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 32, 256, 8, 8, 8, 550, u, &why)) << why;
    EXPECT_EQ(u.luma->pitch, 32u);
    EXPECT_EQ(u.luma->storePitch, 0u);
    EXPECT_EQ(u.chroma->pitch, 1u << 20);
    EXPECT_EQ(u.chroma->storePitch, 16u);
    EXPECT_FALSE(gm20b::xrPlanesCross(gm20b::storedRows(*u.luma, u), gm20b::storedRows(*u.chroma, u)));
    EXPECT_TRUE(gm20b::xrPlanesOverlap(u)) << "stored through its pools";
    dev.destroyTexture(u);
}

TEST_F(DeviceXrSink, APlaneAloneOfWholeRowsHasNoPool)
{
    Surface s;
    gm20b::Texture t;
    const char *why = "";
    ASSERT_TRUE(dev.createXrPlaneTexture(0, s.base, s.bytes, nullptr, 0, 256, 0, 0, 64, 64, 550, t, &why)) << why;
    EXPECT_EQ(t.luma->sampled, nullptr);
    EXPECT_FALSE(gm20b::xrPlanesOverlap(t));
    dev.destroyTexture(t);
}

}  // namespace
