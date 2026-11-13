// SPDX-License-Identifier: MIT
// How a client's ask of the shader compiler goes on after each answer, the same for a state's creation, a variant's
// asynchronous ask and a kernel's (RawCompleteRemote.mm). A record the store lacks after an answer is asked for once more
// with every library sent as its bytes: that one the compiler resolves no system library for, and a record put in the
// store and retired before the asker read it is made again. Whatever is still missing ends the ask; no process compiles
// it itself.
#pragma once
#include "raw_remote.h"

namespace raw {

enum class AskNext : uint8_t {
    Use,       // the store holds the record or its refusal
    AskAgain,  // once more, every library as its bytes
    End,       // no record: the creation fails (askFailure), a variant or kernel is not made
};

enum class AskFailure : uint8_t {
    None,
    CouldNotRun,      // the compiler's compile could not run
    NoCompilerComes,  // the compiler declined, or its launcher stopped starting it
    NotTaken,         // the compiler serves but did not take the request
    NotStored,        // it answered, but the store does not keep the record
};

constexpr AskNext askNext(Remote::Answer answer, bool stored, bool askedAgain, bool declined)
{
    if (stored)
        return AskNext::Use;
    if (answer == Remote::Answer::Failed || askedAgain || declined)
        return AskNext::End;
    return AskNext::AskAgain;
}

constexpr AskFailure askFailure(Remote::Answer answer, bool stored, bool declined)
{
    if (stored)
        return AskFailure::None;
    if (answer == Remote::Answer::Failed)
        return AskFailure::CouldNotRun;
    if (declined)
        return AskFailure::NoCompilerComes;
    return answer == Remote::Answer::Unavailable ? AskFailure::NotTaken : AskFailure::NotStored;
}

}  // namespace raw
