// SPDX-License-Identifier: MIT
// Metal's answers to the predication records as a command buffer takes them (the encode comes later). A unit is a run of
// compute encoders of one dispatch type; any other encoder, an event or the command buffer's end closes it, and with it
// every block still open (m2v's measurements, metal2vulkan-runtime/src/predication.rs). The raw path lays a loop's copies
// across its unit's compute encoders (raw_loop_copies.h): a loop still open where its unit ends is a record it cannot run.
#pragma once
#include <cstdint>
#include <vector>

namespace raw {

class PredicateRecords {
public:
    enum Kind : uint8_t {
        StartIf = 1,
        StartElse,
        EndIf,
        StartWhile,
        EndWhile,
        StartDoWhile,
        EndDoWhile,
    };

    void computeEncoder(uint32_t dispatchType)
    {
        if (!unit_ || type_ != dispatchType) {
            loopAcrossUnits();
            open_.clear();
        }
        unit_ = true;
        type_ = dispatchType;
    }

    void otherWork()
    {
        loopAcrossUnits();
        unit_ = false;
        open_.clear();
    }

    // The command buffer ends
    void end()
    {
        loopAcrossUnits();
    }

    // False: the record is misused (an End with no block of its kind open). It is not the End's answer to its caller,
    // which AGX gives from its Starts' buffers alone (RawMetalCompute.mm)
    bool record(Kind k)
    {
        recorded_ = true;
        switch (k) {
        case StartIf:
            open_.push_back(Block::If);
            return true;
        case StartElse:
            if (open_.empty() || open_.back() != Block::If)
                return misuse();
            open_.back() = Block::Else;
            return true;
        case EndIf:
            if (open_.empty() || open_.back() == Block::Loop)
                return misuse();
            open_.pop_back();
            return true;
        case StartWhile:
        case StartDoWhile:
            loops_ = true;
            open_.push_back(Block::Loop);
            return true;
        case EndWhile:
        case EndDoWhile:
            loops_ = true;
            if (open_.empty() || open_.back() != Block::Loop)
                return misuse();
            open_.pop_back();
            return true;
        }
        return misuse();
    }

    bool recorded() const
    {
        return recorded_;
    }

    // A record with no block of its kind open (Metal kills or drops work by rules raw does not model), or a loop open
    // where its unit ends
    bool misused() const
    {
        return misused_;
    }

    bool loops() const
    {
        return loops_;
    }

    size_t depth() const
    {
        return open_.size();
    }

private:
    enum class Block : uint8_t {
        If,
        Else,
        Loop,
    };

    bool misuse()
    {
        misused_ = true;
        return false;
    }

    void loopAcrossUnits()
    {
        for (Block b : open_)
            if (b == Block::Loop)
                misused_ = true;
    }

    std::vector<Block> open_;
    uint32_t type_ = 0;
    bool unit_ = false, recorded_ = false, misused_ = false, loops_ = false;
};

}  // namespace raw
