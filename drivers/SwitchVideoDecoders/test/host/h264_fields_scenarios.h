// SPDX-License-Identifier: BSD-2-Clause-Patent
// h264_fields_host's streams (h264_fields_stream.h writes them), in decoding order, and what FFmpeg's decoder made of
// each P picture: the pictures its RefPicList0 named, read back from FFmpeg 8.1.2's output by each probe's level
// (`h264_fields_host --oracle <scenario> <yuv>`, the YUV from `ffmpeg -i <h264_fields_host --dump <scenario>> -f
// rawvideo -pix_fmt yuv420p`). "n" is picture n (a field, or a frame picture's top field), "n'" a frame picture's
// bottom field, "t/b" a frame reference's top and bottom field, "-" a non-existing frame's.
#ifndef H264_FIELDS_SCENARIOS_H
#define H264_FIELDS_SCENARIOS_H
#include "h264_fields_stream.h"

typedef struct {
    FieldsPicture picture;
    const char *list0;  // FFmpeg's RefPicList0, for a P picture
} FieldsStep;

#define T FS_TOP
#define B FS_BOTTOM
#define F FS_FRAME

// Top field first; the sliding window drops whole field pairs (8.2.5.3), and a second field joins its first.
static const FieldsStep kTff[] = {
    {{'I', T, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'P', B, false, true, 0, 1, 1, false, NULL, false}, "0"},
    {{'P', T, false, true, 1, 4, 2, false, NULL, true}, "0 1"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "1 2 0"},
    {{'P', T, false, true, 2, 8, 4, false, NULL, true}, "2 3 0 1"},
    {{'P', B, false, true, 2, 9, 3, false, NULL, false}, "3 4 2"},
    {{'P', T, false, true, 3, 12, 4, false, NULL, true}, "4 5 2 3"},
    {{'P', B, false, true, 3, 13, 3, false, NULL, false}, "5 6 4"},
};

// Bottom field first, one field a sample, a non-reference field pair in the middle.
static const FieldsStep kBffSplit[] = {
    {{'I', B, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'P', T, false, true, 0, 1, 1, false, NULL, true}, "0"},
    {{'P', B, false, true, 1, 4, 2, false, NULL, true}, "0 1"},
    {{'P', T, false, true, 1, 5, 3, false, NULL, true}, "1 2 0"},
    {{'P', B, false, false, 2, 8, 4, false, NULL, true}, "2 3 0 1"},
    {{'P', T, false, false, 2, 9, 4, false, NULL, true}, "3 2 1 0"},
    {{'P', B, false, true, 2, 12, 4, false, NULL, true}, "2 3 0 1"},
    {{'P', T, false, true, 2, 13, 3, false, NULL, true}, "3 6 2"},
};

// Frame and field pictures in one stream (PAFF): fields of frame pictures as field references, field pairs as frame
// references.
static const FieldsStep kMixed[] = {
    {{'I', F, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'P', T, false, true, 1, 4, 2, false, NULL, true}, "0 0'"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "0' 1 0"},
    {{'P', F, false, true, 2, 8, 2, false, NULL, true}, "1/2 0/0'"},
    {{'P', T, false, true, 3, 12, 4, false, NULL, true}, "3 3' 1 2"},
    {{'P', B, false, true, 3, 13, 4, false, NULL, false}, "3' 4 2 3"},
    {{'P', F, false, true, 4, 16, 3, false, NULL, true}, "4/5 3/3' 1/2"},
};

// Long-term fields: a field pair turned long-term one field at a time by operation 3 (after 4), and again; operation 2
// one field at a time; 5 in a first field, whose second field then has frame_num 0; 6 on a frame and on a field that
// nothing pairs with. That field's index is above the frame's: a frame's list holds no non-paired field (8.2.4.2.1),
// where FFmpeg lists it and then puts its first entry in its place ("Missing reference picture").
static const FieldsStep kLongTerm[] = {
    {{'I', T, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'P', B, false, true, 0, 1, 1, false, NULL, false}, "0"},
    {{'P', T, false, true, 1, 4, 2, false, "4:2 3:1:0 3:2:0", true}, "0 1"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "2 1 0"},
    {{'P', T, false, true, 2, 8, 4, false, "3:2:1 3:1:1", true}, "2 3 0 1"},
    {{'P', B, false, true, 2, 9, 5, false, NULL, false}, "4 1 0 3 2"},
    {{'P', T, false, true, 3, 12, 4, false, "2:1 2:0", true}, "4 5 0 1"},
    {{'P', B, false, true, 3, 13, 5, false, NULL, false}, "5 6 4 3 2"},
    {{'P', T, false, true, 4, 16, 3, false, "5", true}, "6 7 4"},
    {{'P', B, false, true, 0, 1, 1, false, NULL, false}, "8"},
    {{'P', T, false, true, 1, 4, 2, false, NULL, true}, "8 9"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "9 10 8"},
    {{'I', F, false, true, 2, 8, 0, false, "4:3 1:1 6:1", true}, NULL},
    {{'I', T, false, true, 3, 12, 0, false, "6:2", true}, NULL},
    {{'P', F, false, true, 4, 16, 2, false, NULL, true}, "10/11 12/12'"},
    {{'P', B, false, true, 5, 21, 5, false, NULL, true}, "14' 14 12' 12 13"},
    {{'P', T, false, true, 5, 20, 4, false, NULL, false}, "15 12 12' 13"},
};

// Operations on one field of a pair: 1 unmarks a bottom field and leaves its top field, then the pair before it; 3
// turns the field left over long-term.
static const FieldsStep kFieldOps[] = {
    {{'I', T, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'P', B, false, true, 0, 1, 1, false, NULL, false}, "0"},
    {{'P', T, false, true, 1, 4, 2, false, NULL, true}, "0 1"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "1 2 0"},
    {{'P', T, false, true, 2, 8, 4, false, "1:4", true}, "2 3 0 1"},
    {{'P', B, false, true, 2, 9, 4, false, NULL, false}, "3 4 2 0"},
    {{'P', T, false, true, 3, 12, 4, false, "1:3 1:4 4:1 3:5:0", true}, "4 5 2 3"},
    {{'P', B, false, true, 3, 13, 4, false, NULL, false}, "5 6 4 0"},
};

// An IDR field pair kept long-term: long_term_reference_flag in its first field and operation 6 with the same index in
// its second (8.2.5.4.6: the pair's first field stays marked). These lists are the clause's, not FFmpeg's: FFmpeg 8.1.2
// unmarks the first field there, and the next field's list names only the second (with "Missing reference picture").
static const FieldsStep kLongTermPair[] = {
    {{'I', T, true, true, 0, 0, 0, true, NULL, true}, NULL},
    {{'P', B, false, true, 0, 1, 1, false, "6:0", false}, "0"},
    {{'P', T, false, true, 1, 4, 2, false, NULL, true}, "0 1"},
    {{'P', B, false, true, 1, 5, 3, false, NULL, false}, "2 1 0"},
};

// Fields nothing pairs with (a field before a frame, two top fields in a row, a field at the end), and a frame_num gap
// filled with non-existing frames among field pairs. FFmpeg puts no unpaired field out, so those are I fields here.
static const FieldsStep kUnpaired[] = {
    {{'I', T, true, true, 0, 0, 0, false, NULL, true}, NULL},
    {{'I', F, false, true, 1, 4, 0, false, NULL, true}, NULL},
    {{'I', T, false, true, 2, 8, 0, false, NULL, true}, NULL},
    {{'P', T, false, true, 3, 12, 3, false, NULL, true}, "2 1' 1"},
    {{'P', B, false, true, 3, 13, 2, false, NULL, false}, "3 2"},
    {{'I', T, false, true, 6, 24, 0, false, NULL, true}, NULL},
    {{'I', B, false, true, 6, 25, 0, false, NULL, false}, NULL},
    {{'I', T, false, true, 7, 28, 0, false, NULL, true}, NULL},
    {{'P', T, false, true, 8, 32, 3, false, NULL, true}, "7 6 5"},
    {{'P', B, false, true, 8, 33, 2, false, NULL, false}, "8 7"},
    {{'I', T, false, true, 9, 36, 0, false, NULL, true}, NULL},
};

#undef T
#undef B
#undef F

typedef struct {
    const char *name;
    uint8_t max_num_ref_frames;
    bool gaps;
    const FieldsStep *steps;
    unsigned count;
} FieldsCase;

#define FIELDS_CASE(name, refs, gaps, steps) {name, refs, gaps, steps, sizeof(steps) / sizeof(steps[0])}

static const FieldsCase kFieldsCases[] = {
    FIELDS_CASE("tff", 2, false, kTff),
    FIELDS_CASE("bff_split", 2, false, kBffSplit),
    FIELDS_CASE("mixed", 3, false, kMixed),
    FIELDS_CASE("field_ops", 3, false, kFieldOps),
    FIELDS_CASE("long_term", 3, false, kLongTerm),
    FIELDS_CASE("long_term_pair", 3, false, kLongTermPair),
    FIELDS_CASE("unpaired", 2, true, kUnpaired),
};

#endif
