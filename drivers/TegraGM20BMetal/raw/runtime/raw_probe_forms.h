// SPDX-License-Identifier: MIT
// hw_test_app's barrier probes (app27/X1BarrierProbes.swift), debug builds only: the forms one command buffer's barriers
// take instead of the rules' (raw_barrier_forms.h), so a probe compares a form's pixels and GPU time with today's drain on
// the same work. Bare is the probe brief's reading of an entry with no data invalidates at all; None removes the barrier
// (at a job's entry every invalidate but the instructions'), the arm that shows a probe can see a hazard at all. PIXEL_SHADER_BARRIER is never followed by 0x1288: that reading
// failed on the chip (stack-F-v1), and the method checks of every host test refuse it.
#pragma once
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
#include <cstdint>
#include <cstring>
#include <string>
#include "../../../../kits/SwitchGM20B/SwitchGM20B.h"
#include "../../../../kits/SwitchGM20BEmit/SwitchGM20BEmitBarrier.h"
#include "raw_barrier_forms.h"
#include "raw_push.h"

namespace raw {

// Rule where the probe names no form: production's
struct ProbeForms : BarrierForms {
    bool resolveTwoD = false;
    // an indexed draw's index fetch ends this many bytes past its offset (SET_INDEX_BUFFER_C/D), its indices never
    // padded: the hardware's answer past the limit is the draw's (0: today's)
    uint32_t indexLimit = 0;
    // a storage form's draws take its barrier twin wherever it has one (HW Test gpu.raw.xr_two_plane's barrier cells)
    bool barrierFetch = false;

    bool any() const
    {
        return pass != PassForm::Rule || entry != EntryForm::Rule || switchForm != SwitchForm::Rule || resolveTwoD ||
               indexLimit || barrierFetch;
    }
};

// Where a command buffer's barriers took a form other than today's drain, forced or by a rule (0 everywhere: its work
// never met one)
struct ProbeTaken {
    uint32_t passes = 0, entries = 0, firsts = 0, switches = 0, flushes = 0, resolves = 0, ops = 0, indexLimits = 0;

    void count(Opened o)
    {
        firsts += o == Opened::First;
        passes += o == Opened::Pass;
        switches += o == Opened::Switch;
    }

    void add(const ProbeTaken &o)
    {
        passes += o.passes, entries += o.entries, firsts += o.firsts, switches += o.switches;
        flushes += o.flushes, resolves += o.resolves, ops += o.ops, indexLimits += o.indexLimits;
    }

    std::string text() const
    {
        return "passes=" + std::to_string(passes) + " entries=" + std::to_string(entries) +
               " firsts=" + std::to_string(firsts) + " switches=" + std::to_string(switches) +
               " flushes=" + std::to_string(flushes) + " resolves=" + std::to_string(resolves) +
               " ops=" + std::to_string(ops) + " index_limits=" + std::to_string(indexLimits);
    }
};

// "pass=<today|pixel|none> entry=<today|planner|bare|none> switch=<today|nowfi|nowfi_noflush|none>
// resolve=<draw|twod> index=<bytes> fetch=<today|barrier>", any subset in any order; false on anything else
inline bool parseProbeForms(const char *text, ProbeForms &out)
{
    ProbeForms f;
    std::string s(text ? text : "");
    size_t at = 0;
    while (at < s.size()) {
        const size_t end = std::min(s.find(' ', at), s.size());
        const std::string field = s.substr(at, end - at);
        at = end + 1;
        if (field.empty())
            continue;
        const size_t eq = field.find('=');
        if (eq == std::string::npos)
            return false;
        const std::string key = field.substr(0, eq), value = field.substr(eq + 1);
        if (key == "pass" && value == "today")
            f.pass = PassForm::Today;
        else if (key == "pass" && value == "pixel")
            f.pass = PassForm::PixelOrdered;
        else if (key == "pass" && value == "none")
            f.pass = PassForm::None;
        else if (key == "entry" && value == "today")
            f.entry = EntryForm::Today;
        else if (key == "entry" && value == "planner")
            f.entry = EntryForm::Planner;
        else if (key == "entry" && value == "bare")
            f.entry = EntryForm::Bare;
        else if (key == "entry" && value == "none")
            f.entry = EntryForm::None;
        else if (key == "switch" && value == "today")
            f.switchForm = SwitchForm::Today;
        else if (key == "switch" && value == "nowfi")
            f.switchForm = SwitchForm::NoWfi;
        else if (key == "switch" && value == "nowfi_noflush")
            f.switchForm = SwitchForm::NoWfiNoFlush;
        else if (key == "switch" && value == "none")
            f.switchForm = SwitchForm::None;
        else if (key == "resolve" && (value == "draw" || value == "twod"))
            f.resolveTwoD = value == "twod";
        else if (key == "index" && !value.empty() && value.size() < 9 &&
                 value.find_first_not_of("0123456789") == std::string::npos && std::stoul(value))
            f.indexLimit = uint32_t(std::stoul(value));
        else if (key == "fetch" && (value == "today" || value == "barrier"))
            f.barrierFetch = value == "barrier";
        else
            return false;
    }
    out = f;
    return true;
}

}  // namespace raw
#endif
