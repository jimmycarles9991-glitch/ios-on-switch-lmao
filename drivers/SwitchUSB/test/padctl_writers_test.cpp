// Every write, anywhere in drivers/ and kits/, to an XUSB pad controller register that more than one driver writes
// goes through the pad controller lock (kits/SwitchCAR/SwitchPadctl.h): inside a function that takes it before the
// write (SwitchPadctlLocked, switch_padctl_lock()), one documented "lock held" by its callers, or switch_padctl_modify.
// A new writer of one of these registers that does not take the lock fails here, whichever driver it is in.
#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

// Register names as the drivers spell them (XUSB_PADCTL_ELPG_PROGRAM_1, PADCTL_USB3_PAD_MUX, XUSB_USB2_VBUS_ID, ...).
const char *const kSharedRegisters[] = {"USB2_PAD_MUX", "SS_PORT_MAP",  "USB2_PORT_CAP", "USB2_VBUS_ID", "ELPG_PROGRAM",
                                        "USB3_PAD_MUX", "OTG_PAD0_CTL", "CHRG_OTGPAD",   "BIAS_PAD_CTL", "USB2_OC_MAP"};

bool identifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool namesSharedRegister(const std::string &statement)
{
    return std::ranges::any_of(kSharedRegisters, [&](const char *name) {
        return statement.find(name) != std::string::npos;
    });
}

// switch_mmio_write32/rmw32, an Mmio's modify/w32, or a file's own `w(` helper.
bool writes(const std::string &statement)
{
    for (const char *call : {"switch_mmio_write32(", "switch_mmio_rmw32(", ".modify(", ".w32("}) {
        if (statement.find(call) != std::string::npos)
            return true;
    }
    for (size_t at = statement.find("w("); at != std::string::npos; at = statement.find("w(", at + 1)) {
        if (at == 0 || (!identifierChar(statement[at - 1]) && statement[at - 1] != '.'))
            return true;
    }
    return false;
}

std::string trimmedTail(std::string s)
{
    s.erase(s.find_last_not_of(" \t\n") + 1);
    return s;
}

// What a file-scope `{` opens: a namespace or extern "C" block (looked through), a function, or anything else.
bool looksThrough(const std::string &head)
{
    const std::string h = trimmedTail(head);
    if (h.ends_with("extern"))
        return true;  // extern "C": the literal is blanked
    const size_t ns = h.rfind("namespace");
    return ns != std::string::npos && std::ranges::all_of(h.substr(ns + 9), [](char c) {
               return identifierChar(c) || c == ':' || c == ' ' || c == '\t' || c == '\n';
           });
}

bool opensFunction(const std::string &head)
{
    std::string h = trimmedTail(head);
    for (bool stripped = true; stripped;) {
        stripped = false;
        for (const char *word : {"const", "override", "noexcept", "final"}) {
            const size_t n = strlen(word);
            if (h.size() > n && h.ends_with(word) && !identifierChar(h[h.size() - n - 1])) {
                h = trimmedTail(h.substr(0, h.size() - n));
                stripped = true;
            }
        }
    }
    return !h.empty() && h.back() == ')';
}

struct Function {
    std::string name, comment, body;
};

// Comments and string literals blanked (their text never counts as code), lengths kept.
std::string codeOnly(const std::string &text)
{
    std::string out = text;
    for (size_t i = 0; i < out.size(); ++i) {
        size_t end = 0;
        if (out.compare(i, 2, "//") == 0) {
            end = std::min(out.find('\n', i), out.size());
        } else if (out.compare(i, 2, "/*") == 0) {
            end = std::min(out.find("*/", i + 2), out.size() - 2) + 2;
        } else if (out[i] == '"') {
            end = i + 1;
            while (end < out.size() && out[end] != '"')
                end += out[end] == '\\' ? 2 : 1;
            end = std::min(end + 1, out.size());
        } else {
            continue;
        }
        std::fill(out.begin() + static_cast<long>(i), out.begin() + static_cast<long>(end), ' ');
        i = end - 1;
    }
    return out;
}

// The `//` lines right above `at` in the original text.
std::string commentAbove(const std::string &text, size_t at)
{
    std::string comment;
    size_t lineStart = text.rfind('\n', at == 0 ? 0 : at - 1);
    while (lineStart != std::string::npos && lineStart > 0) {
        const size_t prev = text.rfind('\n', lineStart - 1);
        const size_t from = prev == std::string::npos ? 0 : prev + 1;
        const std::string line = text.substr(from, lineStart - from);
        const size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line.compare(first, 2, "//") != 0)
            break;
        comment.insert(0, line + "\n");
        if (prev == std::string::npos)
            break;
        lineStart = prev;
    }
    return comment;
}

// Just past the `}` that closes the block opening at `open`.
size_t blockEnd(const std::string &code, size_t open)
{
    int depth = 0;
    for (size_t at = open; at < code.size(); ++at) {
        if (code[at] == '{')
            ++depth;
        if (code[at] == '}' && --depth == 0)
            return at + 1;
    }
    return code.size();
}

// The functions defined at file scope (namespace and extern "C" blocks are looked through), each with the comment
// right above it.
std::vector<Function> functions(const std::string &text)
{
    const std::string code = codeOnly(text);
    std::vector<Function> found;
    std::vector<bool> blocks;  // open blocks, true when looked through
    size_t statementStart = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == ';') {
            statementStart = i + 1;
            continue;
        }
        if (c == '}') {
            if (!blocks.empty())
                blocks.pop_back();
            statementStart = i + 1;
            continue;
        }
        if (c != '{')
            continue;
        const std::string head = code.substr(statementStart, i - statementStart);
        const bool transparent = looksThrough(head);
        const bool atFileScope = std::ranges::all_of(blocks, [](bool through) {
            return through;
        });
        if (atFileScope && !transparent && opensFunction(head)) {
            const size_t end = blockEnd(code, i);
            const size_t headStart = code.find_first_not_of(" \t\n", statementStart);
            Function f;
            f.name = code.substr(headStart, code.find('(', headStart) - headStart);
            f.comment = commentAbove(text, headStart);
            f.body = code.substr(i, end - i);
            found.push_back(f);
            i = end - 1;
            statementStart = end;
            continue;
        }
        blocks.push_back(transparent);
        statementStart = i + 1;
    }
    return found;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return s;
}

// "file: function: statement" for each write to a shared register not under the lock; `locked` counts the others.
std::vector<std::string> unlockedWrites(const std::string &file, const std::string &text, unsigned *locked)
{
    std::vector<std::string> bad;
    for (const Function &f : functions(text)) {
        const bool heldByCaller = lower(f.comment).find("lock held") != std::string::npos;
        size_t takes = std::string::npos;
        for (const char *marker : {"SwitchPadctlLocked", "switch_padctl_lock("})
            takes = std::min(takes, f.body.find(marker));
        size_t start = 0;
        while (start < f.body.size()) {
            const size_t end = std::min(f.body.find(';', start), f.body.size());
            const std::string statement = f.body.substr(start, end - start);
            const bool viaModify = statement.find("switch_padctl_modify(") != std::string::npos;
            if ((viaModify || writes(statement)) && namesSharedRegister(statement)) {
                if (viaModify || heldByCaller || takes < start) {
                    ++*locked;
                } else {
                    std::string line = file;
                    line.append(": ").append(f.name).append(": ");
                    line.append(statement.substr(statement.find_first_not_of(" \t\n{")));
                    bad.push_back(line);
                }
            }
            start = end + 1;
        }
    }
    return bad;
}

std::string read(const fs::path &path)
{
    const std::ifstream in(path, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

TEST(PadctlWriters, TheScanSeesAnUnlockedWrite)
{
    unsigned locked = 0;
    const std::string unlocked = "void up()\n{\n    switch_mmio_rmw32(padctl, SS_PORT_MAP, 0xfu, 0);\n}\n";
    EXPECT_EQ(unlockedWrites("x.cpp", unlocked, &locked).size(), 1u);
    const std::string guarded =
        "void up()\n{\n    const SwitchPadctlLocked locked;\n    p.modify(PADCTL_USB3_PAD_MUX, 1, 0);\n}\n";
    EXPECT_TRUE(unlockedWrites("x.cpp", guarded, &locked).empty());
    const std::string held =
        "// Pad controller lock held.\nvoid steps()\n{\n    w(XUSB_PADCTL_ELPG_PROGRAM_1, 0);\n}\n";
    EXPECT_TRUE(unlockedWrites("x.cpp", held, &locked).empty());
    const std::string late = "void up()\n{\n    w(SS_PORT_MAP, 0);\n    const SwitchPadctlLocked locked;\n}\n";
    EXPECT_EQ(unlockedWrites("x.cpp", late, &locked).size(), 1u);
    EXPECT_EQ(locked, 2u);
}

TEST(PadctlWriters, EveryWriterInTheTreeTakesTheLock)
{
    std::vector<std::string> bad;
    std::map<std::string, unsigned> lockedPerFile;
    for (const char *root : {"drivers", "kits"}) {
        ASSERT_TRUE(fs::is_directory(root)) << root;
        for (const fs::directory_entry &entry : fs::recursive_directory_iterator(root)) {
            const std::string path = entry.path().generic_string();
            const std::string ext = entry.path().extension().string();
            if (!entry.is_regular_file() || path.find("/test/") != std::string::npos ||
                (ext != ".c" && ext != ".cpp" && ext != ".h" && ext != ".mm"))
                continue;
            unsigned locked = 0;
            for (const std::string &line : unlockedWrites(path, read(entry.path()), &locked))
                bad.push_back(line);
            if (locked)
                lockedPerFile[path] = locked;
        }
    }
    for (const std::string &line : bad)
        ADD_FAILURE() << "pad controller register written without the lock: " << line;
    // The known writers were seen, so the scan is not passing for want of finding anything.
    for (const char *writer : {"drivers/SwitchUSB/XudcHardware.cpp", "drivers/SwitchUSB/XusbHostPower.cpp",
                               "drivers/SwitchUSB/XusbUtmiPad.cpp", "drivers/SwitchUSB/XusbSuperSpeed.cpp",
                               "drivers/SwitchUSB/XusbPadctl.cpp", "drivers/SwitchWLAN/TegraPCIe.cpp"})
        EXPECT_GT(lockedPerFile[writer], 0u) << writer;
}
}  // namespace
