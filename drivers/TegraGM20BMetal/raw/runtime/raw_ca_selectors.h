// SPDX-License-Identifier: MIT
// DEVELOPMENT: the configs Core Animation's generic merge states (..Xghfc, Pipeline::genericState) are drawn with. A config
// is the FragmentShaderConfig the uber shader branches on: the bytes of the layout (precompile/ca-selectors/<build>.txt,
// raw_ca_selector_layout.h) at its fragment buffer's bind offset, compared without its padding, which holds stale arena
// bytes. Per state: each 5 s window's draws and distinct configs, and each config the process draws the first time, its
// bytes as bound; whether variants of the generic program with the config folded would cover the states' draws, and for
// which configs to compile them.
#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace raw {

constexpr size_t kCaSelectorMax = 64;

struct CaSelectorLayout {
    uint8_t slot = 0;
    uint8_t length = 0;    // 0: the build names none
    uint64_t padding = 0;  // bit i: byte i is padding
};

struct CaSelector {
    uint64_t hash = 0;                   // FNV-1a 64 of its bytes, its padding read as 0
    uint8_t bound[kCaSelectorMax] = {};  // its bytes as bound, padding included
};

// The config in the `size` bound bytes at `cpu`; false where the CPU does not map them or they are fewer than the layout's
inline bool caSelector(const CaSelectorLayout &layout, const uint8_t *cpu, size_t size, CaSelector &out)
{
    if (!layout.length || !cpu || size < layout.length)
        return false;
    out.hash = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < layout.length; i++) {
        out.bound[i] = cpu[i];
        out.hash = (out.hash ^ (layout.padding >> i & 1 ? 0 : cpu[i])) * 0x100000001b3ull;
    }
    return true;
}

class CaSelectorLog {
public:
    static constexpr uint64_t kWindowUs = 5000000;
    static constexpr size_t kStates = 16, kConfigsPerState = 256;

    // One state's closed window, and its process totals at the close
    struct Window {
        uint64_t keyHash = 0;
        uint64_t draws = 0;
        uint64_t unmapped = 0;  // draws whose config the CPU does not map, or of fewer bytes than the layout's
        uint64_t beyond = 0;    // draws of configs past the process's first kConfigsPerState: counted, not told apart
        size_t configs = 0;
        size_t fresh = 0;  // of its configs, those the process drew the first time in it
        uint64_t most = 0, mostConfig = 0;
        size_t processConfigs = 0;
        uint64_t processDraws = 0;
    };

    // A config the process drew the first time: the state's ordinal-th
    struct Fresh {
        uint64_t keyHash = 0;
        size_t ordinal = 0;
        CaSelector selector;
    };

    struct Events {
        std::vector<Window> closed;
        std::vector<Fresh> fresh;
    };

    // A draw of state `keyHash` at `nowUs` with `selector` (null: unmapped), after closing every window that ended by `nowUs`;
    // true where the draw opened its state's window, which a flush() at its end closes
    bool note(uint64_t keyHash, const CaSelector *selector, uint64_t nowUs, Events &out)
    {
        flush(nowUs, out);
        auto found = states_.find(keyHash);
        if (found == states_.end()) {
            if (states_.size() >= kStates)
                return false;
            found = states_.emplace(keyHash, State{}).first;
            found->second.process.reserve(kConfigsPerState);
        }
        State &s = found->second;
        const bool opened = !s.open;
        if (opened) {
            s.open = true;
            s.began = nowUs;
        }
        s.draws++;
        s.processDraws++;
        if (!selector) {
            s.unmapped++;
            return opened;
        }
        if (!s.process.count(selector->hash)) {
            if (s.process.size() >= kConfigsPerState) {
                s.beyond++;
                return opened;
            }
            s.process.emplace(selector->hash, 0);
            s.fresh++;
            out.fresh.push_back({keyHash, s.process.size(), *selector});
        }
        s.process[selector->hash]++;
        s.window[selector->hash]++;
        return opened;
    }

    void flush(uint64_t nowUs, Events &out)
    {
        for (auto &[keyHash, s] : states_)
            if (s.open && nowUs - s.began >= kWindowUs)
                out.closed.push_back(close(keyHash, s));
    }

private:
    struct State {
        bool open = false;
        uint64_t began = 0, draws = 0, unmapped = 0, beyond = 0, processDraws = 0;
        size_t fresh = 0;
        std::unordered_map<uint64_t, uint64_t> window, process;  // config hash: its draws
    };

    static Window close(uint64_t keyHash, State &s)
    {
        Window w;
        w.keyHash = keyHash;
        w.draws = s.draws;
        w.unmapped = s.unmapped;
        w.beyond = s.beyond;
        w.configs = s.window.size();
        w.fresh = s.fresh;
        for (const auto &[config, draws] : s.window)
            if (draws > w.most) {
                w.most = draws;
                w.mostConfig = config;
            }
        w.processConfigs = s.process.size();
        w.processDraws = s.processDraws;
        s.open = false;
        s.draws = s.unmapped = s.beyond = 0;
        s.fresh = 0;
        s.window.clear();
        return w;
    }

    std::unordered_map<uint64_t, State> states_;
};

}  // namespace raw
