// SPDX-License-Identifier: MIT
// The states and the kernel compiler_stages_test and compiler_concurrency_test compile from their test library
// (testdata/compiler_stages/stages.metallib, SWITCH_STAGES_METALLIB) and the crawl's system volumes
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "compiler_requests.h"
#include "../compute_key.h"

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr const char *kLibrary = "/test/compiler_stages.metallib";
constexpr size_t kKept = 64u << 20;

std::string dir()
{
    const char *tmp = getenv("TEST_TMPDIR");
    return tmp ? tmp : "/tmp";
}

std::string root()
{
    return compiler_test::systemRoot(getenv("SWITCH_QUARTZCORE"));
}

std::string library()
{
    const char *path = getenv("SWITCH_STAGES_METALLIB");
    return path ? path : "";
}

struct Case {
    const char *name;
    std::string line;
    // false: the field does not reach the vertex stage's, the fragment stage's compile, or m2v's translation (each
    // measured; true leaves it unjudged, so a translator that keys less stays green)
    bool vertex, fragment, translated;
};

const std::string kVertex = " | v stages_vertex lib=" + std::string(kLibrary);
const std::string kFragment = " | f stages_fragment lib=" + std::string(kLibrary);
const std::string kFetch = " | f stages_fetch_fragment lib=" + std::string(kLibrary);
const std::string kAttributes = " a0=31:0:0:24:1:1 a1=29:16:0:24:1:1";
const std::string kBlended = "c0=80:15:1:4:5:0:4:5:0";

std::string state(const std::string &head, const std::string &colours, const std::string &attributes = kAttributes,
                  const std::string &fragment = kFragment)
{
    return "R ok=1 " + head + " " + colours + attributes + kVertex + fragment;
}

// The base state first, then one field changed each, as compiler_stages_test's table measured them: the vertex stage's
// compile reads the blend switch (its push words and layout), and the fragment stage's the blend switch, a 16-bit
// float target, the sample count and, fetching its attachment, the write mask; the colour format within a class, the
// blend factors and operations, the topology class, a depth attachment and every attribute layout reach neither
std::vector<Case> renderCases()
{
    const std::string plain = "samples=1 topo=0 ds=0/0";
    return {
        {"base", state(plain, kBlended), true, true, true},
        {"colour format BGRA8 -> RGBA8", state(plain, "c0=70:15:1:4:5:0:4:5:0"), false, false, false},
        {"colour format -> RGBA16Float", state(plain, "c0=115:15:1:4:5:0:4:5:0"), false, true, false},
        {"colour format -> R8Unorm", state(plain, "c0=10:15:1:4:5:0:4:5:0"), false, false, true},
        {"write mask", state(plain, "c0=80:7:1:4:5:0:4:5:0"), false, false, false},
        {"blending off", state(plain, "c0=80:15:0:4:5:0:4:5:0"), true, true, true},
        {"source RGB factor", state(plain, "c0=80:15:1:1:5:0:4:5:0"), false, false, false},
        {"destination RGB factor", state(plain, "c0=80:15:1:4:1:0:4:5:0"), false, false, false},
        {"RGB operation", state(plain, "c0=80:15:1:4:5:1:4:5:0"), false, false, false},
        {"alpha factors and operation", state(plain, "c0=80:15:1:1:1:1:4:5:0"), false, false, false},
        {"a second colour attachment", state(plain, kBlended + " c1=80:15:0:1:0:0:1:0:0"), false, false, true},
        {"four samples", state("samples=4 topo=0 ds=0/0", kBlended), false, true, true},
        {"topology class", state("samples=1 topo=3 ds=0/0", kBlended), false, false, false},
        {"depth attachment", state("samples=1 topo=0 ds=252/0", kBlended), false, false, false},
        {"buffer stride", state(plain, kBlended, " a0=31:0:0:48:1:1 a1=29:16:0:48:1:1"), false, false, false},
        {"attribute offset", state(plain, kBlended, " a0=31:0:0:32:1:1 a1=29:20:0:32:1:1"), false, false, false},
        {"attribute format", state(plain, kBlended, " a0=31:0:0:24:1:1 a1=25:16:0:24:1:1"), false, false, false},
        {"fetch base", state(plain, "c0=80:15:0:1:0:0:1:0:0", kAttributes, kFetch), true, true, true},
        {"fetch format BGRA8 -> RGBA8", state(plain, "c0=70:15:0:1:0:0:1:0:0", kAttributes, kFetch), false, false,
         true},
        {"fetch blending on", state(plain, kBlended, kAttributes, kFetch), true, true, true},
        {"fetch write mask", state(plain, "c0=80:7:0:1:0:0:1:0:0", kAttributes, kFetch), true, true, true},
    };
}

gm20b::CompileRequest kernelRequest(const std::string &bytes, gm20b::RequestKind kind, const uint32_t threads[3],
                                    uint64_t format)
{
    std::map<std::string, gm20b::AirHash> hashes;
    gm20b::metallibAirHashes(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size(), hashes);
    gm20b::CompileRequest r;
    r.kind = kind;
    gm20b::RequestLibrary l;
    l.path = kLibrary;
    l.bytes = bytes;
    r.libraries.push_back(l);
    gm20b::RequestStage s;
    s.tag = "k";
    s.function = s.name = "stages_kernel";
    r.stages.push_back(s);
    const gm20b::AirHash &air = hashes["stages_kernel"];
    const std::string state = gm20b::computeStateKey(air, "", 0, false);
    if (kind == gm20b::RequestKind::Facts) {
        r.key = gm20b::factsCacheKey(0, state).key;
        return r;
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = MTLPixelFormat(format);
    d.textureType = MTLTextureType2D;
    d.usage = MTLTextureUsageShaderRead;
    gm20b::KernelDispatch dispatch(threads, {});
    dispatch.texture(0, d);
    const NmbDirectDispatch &nd = dispatch.dispatch();
    memcpy(r.kernel.threads, nd.threads, sizeof r.kernel.threads);
    r.kernel.textures.assign(nd.textures, nd.textures + nd.textureCount);
    r.key = "kernel | " +
            gm20b::computeKey(air, "", 0, false, threads, "", "0:" + std::to_string(format) + ":2:1:1:02030405", "");
    return r;
}

std::string fileBytes(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace
