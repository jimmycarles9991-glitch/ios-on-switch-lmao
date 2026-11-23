// SPDX-License-Identifier: MIT
// remote_ask.h: how a client's ask of the shader compiler goes on after each answer, for every answer,
// with the record stored or not, asked again or not and the compiler declined or not. No path of it compiles in the client:
// what is still missing after the second ask ends it, with the reason the creation fails with.
#include "../remote_ask.h"

#include <gtest/gtest.h>

namespace {

using gm20b::AskFailure;
using gm20b::askFailure;
using gm20b::AskNext;
using gm20b::askNext;
using Answer = gm20b::Remote::Answer;

constexpr Answer kAnswers[] = {Answer::Made, Answer::Refused, Answer::Failed, Answer::Unavailable};

TEST(RemoteAsk, AStoredRecordIsUsedWhateverTheAnswer)
{
    for (Answer answer : kAnswers)
        for (bool again : {false, true})
            for (bool declined : {false, true}) {
                EXPECT_EQ(askNext(answer, true, again, declined), AskNext::Use);
                EXPECT_EQ(askFailure(answer, true, declined), AskFailure::None);
            }
}

// The compiler resolves no system library of the request: sent again with every library as its bytes
TEST(RemoteAsk, ARequestTheCompilerDidNotTakeIsAskedOnceMoreThenFails)
{
    EXPECT_EQ(askNext(Answer::Unavailable, false, false, false), AskNext::AskAgain);
    EXPECT_EQ(askNext(Answer::Unavailable, false, true, false), AskNext::End);
    EXPECT_EQ(askFailure(Answer::Unavailable, false, false), AskFailure::NotTaken);
}

// Put in the store and retired before the asker read it: made again once
TEST(RemoteAsk, ARecordTheStoreLostIsAskedOnceMoreThenFails)
{
    for (Answer answer : {Answer::Made, Answer::Refused}) {
        EXPECT_EQ(askNext(answer, false, false, false), AskNext::AskAgain);
        EXPECT_EQ(askNext(answer, false, true, false), AskNext::End);
        EXPECT_EQ(askFailure(answer, false, false), AskFailure::NotStored);
    }
}

TEST(RemoteAsk, ACompileThatCouldNotRunEndsAtOnce)
{
    for (bool again : {false, true})
        EXPECT_EQ(askNext(Answer::Failed, false, again, false), AskNext::End);
    EXPECT_EQ(askFailure(Answer::Failed, false, false), AskFailure::CouldNotRun);
    EXPECT_EQ(askFailure(Answer::Failed, false, true), AskFailure::CouldNotRun);
}

TEST(RemoteAsk, NoCompilerComingEndsWithNoSecondAsk)
{
    for (Answer answer : {Answer::Made, Answer::Refused, Answer::Unavailable}) {
        EXPECT_EQ(askNext(answer, false, false, true), AskNext::End);
        EXPECT_EQ(askFailure(answer, false, true), AskFailure::NoCompilerComes);
    }
}

}  // namespace
