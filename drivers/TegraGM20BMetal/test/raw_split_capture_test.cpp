// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include "drivers/TegraGM20BMetal/raw/runtime/raw_split_capture.h"

namespace {

// The record tools/benchloop/capture_test.py reads back from testdata/split_capture.log: Iscd's key, its 832x1408
// RGBA16Float target, a strip of four 16-byte vertices, a fragment block at residual 80 and a vertex block at 0
raw::SplitCaptureRecord known()
{
    raw::SplitCaptureRecord r;
    r.key = 0x618b45175101cd20ull;
    r.ca = 1;
    r.format = 115, r.width = 832, r.height = 1408;
    r.box[0] = 20, r.box[1] = 954, r.box[2] = 129, r.box[3] = 1280;
    r.primitive = 4, r.first = 0, r.count = 4, r.stride = 16;
    for (uint32_t i = 0; i < 32; i++)
        r.push[i] = i * 3;
    for (uint32_t i = 0; i < 64; i++)
        r.vertices.push_back(uint8_t(255 - i));
    raw::SplitCaptureBlock fragment{2, 1, 80, {}};
    for (uint32_t i = 0; i < 64; i++)
        fragment.bytes.push_back(uint8_t(i));
    raw::SplitCaptureBlock vertex{1, 0, 0, std::vector<uint8_t>(64, 0x3c)};
    r.blocks = {fragment, vertex};
    return r;
}

std::vector<std::string> golden()
{
    const char *dir = getenv("TEST_SRCDIR");
    std::ifstream in(std::string(dir ? dir : ".") + "/_main/tools/benchloop/testdata/split_capture.log");
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);)
        lines.push_back(line.substr(line.find("splitcap ")));
    return lines;
}

}  // namespace

TEST(RawSplitCapture, LinesCarryTheRecordWhole)
{
    const std::vector<std::string> lines = raw::splitCaptureLines(known(), 7);
    ASSERT_GE(lines.size(), 2u);
    for (const std::string &line : lines)
        EXPECT_LE(line.size() + strlen("raw: "), raw::kSplitCaptureLine);
    raw::SplitCaptureRecord back;
    ASSERT_TRUE(raw::splitCaptureFromLines(lines, back));
    EXPECT_TRUE(back == known());
}

// The bench's capture tool reads these lines: the device writes them as the golden file has them
TEST(RawSplitCapture, LinesAreTheGoldenFiles)
{
    EXPECT_EQ(raw::splitCaptureLines(known(), 7), golden());
}

// The most a record holds stays inside the bound the header states, and a block past its cap is no record
TEST(RawSplitCapture, ARecordIsBounded)
{
    raw::SplitCaptureRecord r = known();
    r.vertices.assign(raw::kSplitCaptureVertexBytes, 1);
    r.blocks.assign(raw::kSplitCaptureBlocks, raw::SplitCaptureBlock{2, 3, 255, std::vector<uint8_t>(2048, 2)});
    const std::vector<uint8_t> blob = raw::splitCaptureBlob(r);
    EXPECT_EQ(blob.size(), 20808u);
    raw::SplitCaptureRecord back;
    ASSERT_TRUE(raw::splitCaptureRecord(blob.data(), blob.size(), back));
    EXPECT_TRUE(back == r);
    r.blocks[0].bytes.push_back(0);
    const std::vector<uint8_t> over = raw::splitCaptureBlob(r);
    EXPECT_FALSE(raw::splitCaptureRecord(over.data(), over.size(), back));
    EXPECT_FALSE(raw::splitCaptureRecord(blob.data(), blob.size() - 1, back));
}
