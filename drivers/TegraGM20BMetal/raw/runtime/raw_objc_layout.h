// SPDX-License-Identifier: MIT
// A struct ivar's fields from its Objective-C type encoding ({Name="field"type...}), laid out as C lays them out: each
// field's offset and size, an object field's kind '@'. The runtime keeps the encoding of every ivar of Metal's own classes,
// so a struct of theirs is read by its field names (RawMetalPassCache.mm), not by offsets taken from one SDK.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace raw::objc {

struct Field {
    std::string_view name;
    size_t offset, size;
    char kind;  // the encoding's code: '@' an object, '{' a struct, '[' an array, '(' a union, else a scalar's
};

namespace detail {

struct Cursor {
    std::string_view s;
    size_t at = 0;

    char peek() const
    {
        return at < s.size() ? s[at] : '\0';
    }
};

inline size_t alignUp(size_t v, size_t a)
{
    return a ? (v + a - 1) / a * a : v;
}

bool parse(Cursor &c, size_t &size, size_t &align, std::vector<Field> *fields);

// A quoted name at the cursor; false: none there
inline bool quoted(Cursor &c, std::string_view &name)
{
    if (c.peek() != '"')
        return false;
    const size_t end = c.s.find('"', c.at + 1);
    if (end == std::string_view::npos)
        return false;
    name = c.s.substr(c.at + 1, end - c.at - 1);
    c.at = end + 1;
    return true;
}

// The fields of a struct or union body up to `close`, each optionally named
inline bool body(Cursor &c, char close, bool isUnion, size_t &size, size_t &align, std::vector<Field> *fields)
{
    size = 0;
    align = 1;
    while (c.peek() != close) {
        if (!c.peek())
            return false;
        std::string_view name;
        quoted(c, name);
        size_t fieldSize, fieldAlign;
        const char kind = c.peek();
        if (!parse(c, fieldSize, fieldAlign, nullptr))
            return false;
        const size_t offset = isUnion ? 0 : alignUp(size, fieldAlign);
        if (fields)
            fields->push_back({name, offset, fieldSize, kind});
        size = isUnion ? (fieldSize > size ? fieldSize : size) : offset + fieldSize;
        align = fieldAlign > align ? fieldAlign : align;
    }
    c.at++;
    size = alignUp(size, align);
    return true;
}

inline bool parse(Cursor &c, size_t &size, size_t &align, std::vector<Field> *fields)
{
    while (c.peek() && std::string_view("rnNoORVA").find(c.peek()) != std::string_view::npos)
        c.at++;
    const char code = c.peek();
    c.at++;
    switch (code) {
    case 'c':
    case 'C':
    case 'B':
        size = align = 1;
        return true;
    case 's':
    case 'S':
        size = align = 2;
        return true;
    case 'i':
    case 'I':
    case 'l':
    case 'L':
    case 'f':
        size = align = 4;
        return true;
    case 'q':
    case 'Q':
    case 'd':
    case '*':
    case '#':
    case ':':
        size = align = 8;
        return true;
    case '@': {
        size = align = 8;
        if (c.peek() == '?') {  // a block
            c.at++;
            return true;
        }
        // a class or protocol name, where the quoted text after it is no type of its own: the next field's name or the end
        if (c.peek() == '"') {
            const size_t end = c.s.find('"', c.at + 1);
            if (end == std::string_view::npos)
                return false;
            const char after = end + 1 < c.s.size() ? c.s[end + 1] : '\0';
            if (after == '"' || after == '}' || after == ')' || after == ']' || after == '\0')
                c.at = end + 1;
        }
        return true;
    }
    case '^': {
        size = align = 8;
        if (c.peek() == '?') {
            c.at++;
            return true;
        }
        // the pointee: a struct named with no body is complete enough here
        if (c.peek() == '{' || c.peek() == '(') {
            int depth = 0;
            do {
                const char ch = c.peek();
                depth += ch == '{' || ch == '(';
                depth -= ch == '}' || ch == ')';
                c.at++;
            } while (depth && c.peek());
            return depth == 0;
        }
        size_t s2, a2;
        return parse(c, s2, a2, nullptr);
    }
    case '[': {
        size_t count = 0;
        while (c.peek() >= '0' && c.peek() <= '9')
            count = count * 10 + size_t(c.peek() - '0'), c.at++;
        size_t element, elementAlign;
        if (!parse(c, element, elementAlign, nullptr) || c.peek() != ']')
            return false;
        c.at++;
        size = element * count;
        align = elementAlign;
        return true;
    }
    case '{':
    case '(': {
        const char close = code == '{' ? '}' : ')';
        const size_t equals = c.s.find('=', c.at);
        const size_t closing = c.s.find(close, c.at);
        if (equals == std::string_view::npos || (closing != std::string_view::npos && closing < equals))
            return false;  // a struct with no fields here: its size is not in the encoding
        c.at = equals + 1;
        return body(c, close, code == '(', size, align, fields);
    }
    default:
        return false;  // a bitfield, void, or a code this reader does not know
    }
}

}  // namespace detail

// The fields of the struct `type` encodes and its size; false where it cannot be laid out from the encoding alone
inline bool structFields(std::string_view type, std::vector<Field> &fields, size_t &size)
{
    detail::Cursor c{type};
    size_t align;
    fields.clear();
    if (c.peek() != '{')
        return false;
    return detail::parse(c, size, align, &fields) && c.at == type.size();
}

// The field named `name` of kind `kind` ('\0': any), null where there is none
inline const Field *field(const std::vector<Field> &fields, std::string_view name, char kind = '\0')
{
    for (const Field &f : fields)
        if (f.name == name && (!kind || f.kind == kind))
            return &f;
    return nullptr;
}

}  // namespace raw::objc
