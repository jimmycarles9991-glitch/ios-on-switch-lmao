// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>

namespace raw {

// How a state's creation got its record (debug builds: the HW Test reads it, RawMetalCompileProbe.mm)
struct Obtained {
    const char *how = "none";  // "pack", "table", "store", "compiled", "joined", "stored", "here"
    uint64_t waitUs = 0;       // the creating thread's wait for the shader compiler
    uint32_t qos = 0;          // that thread's class
    uint32_t ran = 0;          // the compiler's answer: how, and the class it ran at (TegraGM20BCompilerABI.h)
};

}  // namespace raw
