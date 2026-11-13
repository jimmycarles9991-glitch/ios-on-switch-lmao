// SPDX-License-Identifier: MIT
// Corpus lines (precompile/corpus.log's R and D lines, lib= paths on the system volumes) as the compile requests a client
// sends the system shader compiler for their states, over a crawl of the system volumes at `root`; and the libraries an m2v
// device makes of a request, as the compiler made them before its compile context (compiler_engine.mm, x1-footprint3).
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "../compiler/compiler_record.h"
#include "../compiler/compiler_request.h"
#include "../raw/pack/kernel_shapes.h"
#include "../raw/pack/probe_states.h"
#include "../raw/runtime/RawCompileRequest.h"
#include "../raw/runtime/RawDirect.h"
#include "../raw/runtime/raw_render_key.h"

namespace compiler_test {

constexpr const char *kQuartzCore = "/System/Library/Frameworks/QuartzCore.framework/default.metallib";

// The root of the system volumes whose QuartzCore library is the file at `qc`, symbolic links resolved: what
// SystemLibraries and the engine resolve paths under
inline std::string systemRoot(const char *qc)
{
    char real[PATH_MAX];
    if (!qc || !realpath(qc, real))
        return std::string();
    const std::string path = real, tail = kQuartzCore;
    return path.size() > tail.size() && !path.compare(path.size() - tail.size(), tail.size(), tail)
               ? path.substr(0, path.size() - tail.size())
               : std::string();
}

class Requests {
public:
    // `data`: the host's copy of each library off the system volumes a line may name, which a request carries
    explicit Requests(std::string root, std::map<std::string, std::string> data = {})
        : root_(std::move(root)), data_(std::move(data))
    {}

    // An R line's state; false with `why` for one no client could ask of these libraries
    bool render(const std::string &line, raw::CompileRequest &r, std::string &why)
    {
        r = {};
        const size_t v = line.find(" | v ");
        if (v == std::string::npos || !fields(line.substr(0, v), r.fields))
            return why = "no state", false;
        for (size_t at = v; at != std::string::npos && at + 3 < line.size();) {
            const size_t next = line.find(" | ", at + 3);
            const std::string segment = line.substr(at + 3, (next == std::string::npos ? line.size() : next) - at - 3);
            const std::string tag = segment.substr(0, segment.find(' '));
            if (tag == "v" || tag == "f" || tag == "vl" || tag == "vp" || tag == "fl" || tag == "fp") {
                if (!stage(r, tag, segment.substr(tag.size()), why))
                    return false;
            } else if (tag == "cubic" && !probe::cubicBinds(segment, r.cubic)) {
                return why = "a cubic segment it cannot read", false;
            } else if (tag.rfind("tests=", 0) == 0) {
                r.fragmentTests = uint8_t(atoi(tag.c_str() + 6));
            }
            at = next;
        }
        if (r.stages.size() < 2 || r.stages[0].tag != "v" || r.stages[1].tag != "f")
            return why = "no vertex and fragment function", false;
        return renderKey(r, why);
    }

    // A D line's kernel at each region shape its dispatch has
    bool kernels(const std::string &line, std::vector<raw::CompileRequest> &out, std::string &why)
    {
        probe::Shape shape;
        if (!probe::parse(line, shape, why))
            return false;
        raw::CompileRequest r;
        r.kind = raw::RequestKind::Kernel;
        if (!addLibrary(r, shape.library, why))
            return false;
        raw::RequestStage s;
        s.tag = "k";
        s.function = s.name = shape.function;
        s.constants = raw::constantsText(probe::constantValues(shape.constants));
        s.specialized = shape.specialized || !s.constants.empty();
        r.stages.push_back(s);
        const auto air = hashes_[shape.library].find(shape.function);
        if (air == hashes_[shape.library].end())
            return why = "no AIR for " + shape.function, false;
        probe::eachRegionShape(shape, [&](const probe::Shape &region, uint8_t) {
            raw::CompileRequest k = r;
            std::map<uint32_t, uint32_t> memory;
            std::istringstream text(region.threadgroupMemory);
            for (std::string pair; std::getline(text, pair, ',');)
                memory[uint32_t(strtoul(pair.c_str(), nullptr, 10))] =
                    uint32_t(strtoul(pair.c_str() + pair.find(':') + 1, nullptr, 10));
            raw::KernelDispatch dispatch(region.threads, memory, region.regionMode);
            for (const raw::BoundTexture &t : region.textures) {
                MTLTextureDescriptor *d = [MTLTextureDescriptor new];
                d.pixelFormat = MTLPixelFormat(t.pixelFormat);
                d.textureType = MTLTextureType(t.textureType);
                d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
                dispatch.texture(t.slot, d);
            }
            for (const raw::BoundSampler &b : region.samplers)
                dispatch.sampler(b.slot, probe::samplerDescriptor(b));
            const NmbDirectDispatch &d = dispatch.dispatch();
            raw::KernelRequest &q = k.kernel;
            q.maxThreads = region.maxThreads;
            q.multipleOfWidth = region.multipleOfWidth;
            q.regionMode = region.regionMode;
            memcpy(q.threads, d.threads, sizeof q.threads);
            memcpy(q.memory, d.memory, sizeof q.memory);
            q.textures.assign(d.textures, d.textures + d.textureCount);
            q.samplers.assign(d.samplers, d.samplers + d.samplerCount);
            k.key = "kernel | " + probe::key(region, air->second);
            out.push_back(std::move(k));
        });
        return true;
    }

    // Every request `lines` names, in order, each line's either way
    std::vector<raw::CompileRequest> of(const std::vector<std::string> &lines)
    {
        std::vector<raw::CompileRequest> out;
        for (const std::string &line : lines) {
            raw::CompileRequest r;
            std::string why;
            if (line.rfind("R ", 0) == 0 && render(line, r, why))
                out.push_back(std::move(r));
            else if (line.rfind("D ", 0) == 0)
                kernels(line, out, why);
        }
        return out;
    }

private:
    std::string root_;
    std::map<std::string, std::string> data_;
    std::map<std::string, std::map<std::string, raw::AirHash>> hashes_;

    static bool fields(const std::string &head, raw::PipelineFields &f)
    {
        for (const std::string &h : probe::split(head)) {
            unsigned i = 0, x[9] = {};
            if (h.rfind("samples=", 0) == 0)
                f.samples = strtoul(h.c_str() + 8, nullptr, 10);
            else if (h.rfind("topo=", 0) == 0)
                f.topology = strtoul(h.c_str() + 5, nullptr, 10);
            else if (h.rfind("ds=", 0) == 0 && sscanf(h.c_str() + 3, "%u/%u", x, x + 1) == 2) {
                f.depthFormat = x[0];
                f.stencilFormat = x[1];
            } else if (sscanf(h.c_str(), "c%u=%u:%u:%u:%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5,
                              x + 6, x + 7, x + 8) == 10 &&
                       i < 8)
                f.color[i] = {x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8]};
            else if (sscanf(h.c_str(), "a%u=%u:%u:%u:%u:%u:%u", &i, x, x + 1, x + 2, x + 3, x + 4, x + 5) == 7 &&
                     i < 31)
                f.attributes[i] = {x[0], x[1], x[2], x[3], x[4], x[5]};
        }
        return true;
    }

    bool addLibrary(raw::CompileRequest &r, const std::string &path, std::string &why)
    {
        for (uint32_t i = 0; i < r.libraries.size(); ++i)
            if (r.libraries[i].path == path)
                return true;
        raw::RequestLibrary library;
        library.path = path;
        library.system = raw::systemLibraryPath(path);
        if (!library.system) {
            const auto copy = data_.find(path);
            if (copy == data_.end())
                return why = "a library off the system volume: " + path, false;
            std::ifstream in(copy->second, std::ios::binary);
            library.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        if (!hashes_.count(path)) {
            std::map<std::string, raw::AirHash> read;
            const bool parsed = library.system
                                    ? raw::metallibAirHashes(root_ + path, read)
                                    : raw::metallibAirHashes(reinterpret_cast<const uint8_t *>(library.bytes.data()),
                                                             library.bytes.size(), read);
            if (!parsed)
                return why = "no library " + path, false;
            hashes_[path] = std::move(read);
        }
        r.libraries.push_back(std::move(library));
        return true;
    }

    // A stage segment's tokens: its function, lib=, as=, spec and its constants
    bool stage(raw::CompileRequest &r, const std::string &tag, const std::string &text, std::string &why)
    {
        const std::vector<std::string> tokens = probe::split(text);
        if (tokens.empty())
            return why = "an empty stage", false;
        raw::RequestStage s;
        s.tag = tag;
        s.function = s.name = tokens[0];
        std::string path = kQuartzCore, constants;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].rfind("lib=", 0) == 0 || tokens[i].rfind("from=", 0) == 0)
                path = tokens[i].substr(tokens[i].find('=') + 1);
            else if (tokens[i].rfind("as=", 0) == 0)
                s.name = tokens[i].substr(3);
            else if (tokens[i] == "spec")
                s.specialized = true;
            else
                constants += (constants.empty() ? "" : " ") + tokens[i];
        }
        if (!constants.empty()) {
            MTLFunctionConstantValues *values = raw::constantValuesOf(constants);
            if (!values)
                return why = "constants it cannot read", false;
            s.constants = raw::constantsText(values);
            s.specialized = true;
        }
        if (!addLibrary(r, path, why))
            return false;
        for (uint32_t i = 0; i < r.libraries.size(); ++i)
            if (r.libraries[i].path == path)
                s.library = i;
        if (!hashes_[path].count(s.function))
            return why = "no function " + s.function + " in " + path, false;
        r.stages.push_back(s);
        return true;
    }

    // compiler_request.mm renderKey's key, which the client sends as its own
    bool renderKey(raw::CompileRequest &r, std::string &why)
    {
        const auto air = [&](const raw::RequestStage &s) {
            return hashes_[r.libraries[s.library].path][s.function];
        };
        std::vector<raw::LinkedStageAir> linked;
        for (size_t i = 2; i < r.stages.size(); ++i)
            linked.push_back({r.stages[i].tag[0], air(r.stages[i])});
        bool own = false;
        std::string packKey;
        @autoreleasepool {
            packKey = raw::requestPackKey(r, own);
        }
        r.key = raw::renderCacheKey(r.stack, packKey, air(r.stages[0]), air(r.stages[1]), linked, own).key;
        return !r.key.empty() || (why = "no key", false);
    }
};

// The corpus lines of `path`, R and D, in order
inline std::vector<std::string> corpusLines(const std::string &path)
{
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);)
        if ((line.rfind("R ok=1 ", 0) == 0 || line.rfind("D ok=1 ", 0) == 0))
            lines.push_back(line);
    return lines;
}

// A request's libraries as an m2v device makes them: what the compiler made them of before its compile context
inline switch_compiler::Libraries deviceLibraries(id<MTLDevice> device, const std::string &root)
{
    return [device, root](const raw::RequestLibrary &l, std::string &why) -> id<MTLLibrary> {
        NSError *error = nil;
        std::string real;
        if (l.system && !switch_compiler::SystemLibraries::resolve(root, l.path, real, why))
            return nil;
        id<MTLLibrary> made =
            l.system ? [device newLibraryWithURL:[NSURL fileURLWithPath:@(real.c_str())] error:&error]
                     : [device newLibraryWithData:dispatch_data_create(l.bytes.data(), l.bytes.size(), nullptr,
                                                                       DISPATCH_DATA_DESTRUCTOR_DEFAULT)
                                            error:&error];
        if (!made)
            why = "no library " + l.path;
        return made;
    };
}

// A raw pack of no states, which a host's stack (0) takes: rules of nothing (raw_record.h PackRules)
inline std::string emptyPack(const std::string &dir)
{
    std::vector<uint8_t> file(0x400, 0);
    raw::PackHeader h = {};
    memcpy(h.magic, "GM20BPK1", 8);
    h.version = raw::kPackVersion;
    memcpy(file.data(), &h, sizeof h);
    const std::string path = dir + "/pack.bin";
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(file.data()), file.size());
    return path;
}

}  // namespace compiler_test
