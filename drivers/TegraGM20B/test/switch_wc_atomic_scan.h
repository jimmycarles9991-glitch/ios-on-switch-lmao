// SPDX-License-Identifier: MIT
// switch_wc_atomic_source_test's reading of C and C++ source: comments and strings blanked, a call's arguments, the
// atomic read-modify-write calls and AArch64 exclusive or atomic mnemonics, and an operand's memory one hop back
// through the local it names.
#pragma once
#include <cctype>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace wc_atomic_scan
{
inline std::string blanked(const std::string &src)
{
	std::string out = src;
	for (size_t i = 0; i < out.size();) {
		if (out.compare(i, 2, "//") == 0) {
			while (i < out.size() && out[i] != '\n')
				out[i++] = ' ';
		} else if (out.compare(i, 2, "/*") == 0) {
			const size_t end = out.find("*/", i + 2);
			const size_t stop = end == std::string::npos ? out.size() : end + 2;
			for (; i < stop; ++i)
				if (out[i] != '\n')
					out[i] = ' ';
		} else if (out[i] == '"' || out[i] == '\'') {
			const char quote = out[i++];
			while (i < out.size() && out[i] != quote) {
				if (out[i] == '\\' && i + 1 < out.size())
					out[i++] = ' ';
				if (out[i] != '\n')
					out[i] = ' ';
				++i;
			}
			++i;
		} else {
			++i;
		}
	}
	return out;
}

inline std::string firstArgument(const std::string &text, size_t at)
{
	int depth = 0;
	for (size_t i = at; i < text.size(); ++i) {
		const char c = text[i];
		if (c == '(' || c == '[' || c == '{')
			++depth;
		else if ((c == ')' || c == ']' || c == '}') && depth-- == 0)
			return text.substr(at, i - at);
		else if (c == ',' && depth == 0)
			return text.substr(at, i - at);
	}
	return text.substr(at);
}

// A call's arguments at the top level, from just past its '('
inline std::vector<std::string> arguments(const std::string &text, size_t at)
{
	std::vector<std::string> out;
	int depth = 0;
	size_t from = at;
	for (size_t i = at; i < text.size(); ++i) {
		const char c = text[i];
		if (c == '(' || c == '[' || c == '{') {
			++depth;
		} else if ((c == ')' || c == ']' || c == '}') && depth-- == 0) {
			out.push_back(text.substr(from, i - from));
			return out;
		} else if (c == ',' && depth == 0) {
			out.push_back(text.substr(from, i - from));
			from = i + 1;
		}
	}
	out.push_back(text.substr(from));
	return out;
}

// Whether an RMW operand reaches the memory a carrier names: `chan->fence_cpu` or `&words[i]` does, `&gCar` (the
// cached variable that holds the pointer) does not.
inline bool reaches(const std::string &operand, const std::regex &carrier)
{
	size_t first = operand.find_first_not_of(" \t\n");
	size_t last = operand.find_last_not_of(" \t\n");
	if (first == std::string::npos)
		return false;
	const std::string trimmed = operand.substr(first, last - first + 1);
	for (std::sregex_iterator it(trimmed.begin(), trimmed.end(), carrier), end; it != end; ++it)
		if (trimmed[0] != '&' || size_t(it->position() + it->length()) != trimmed.size())
			return true;
	return false;
}

inline bool identifierChar(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// The local an operand names (`at` in `at`, `&at[2]`, `*at`), whose initialization a carrier may be in; "" for none
inline std::string local(const std::string &operand)
{
	size_t i = operand.find_first_not_of(" \t\n&*(");
	size_t end = i;
	while (end < operand.size() && identifierChar(operand[end]))
		++end;
	if (i == std::string::npos || end == i ||
	    std::isdigit(static_cast<unsigned char>(operand[i])))
		return {};
	const std::string rest = operand.substr(end);
	return rest.find_first_not_of(" \t\n)") == std::string::npos ||
			       rest.find_first_of("[+") == rest.find_first_not_of(" \t\n")
		       ? operand.substr(i, end - i)
		       : std::string();
}

// The expression the nearest assignment before `at` gives `name` (one hop back, within its function's reach)
inline std::string assigned(const std::string &source, size_t at, const std::string &name)
{
	const size_t from = at > 4000 ? at - 4000 : 0;
	const std::regex assignment("\\b" + name + "\\s*=[^=]");
	std::string found;
	const std::string window = source.substr(from, at - from);
	for (std::sregex_iterator it(window.begin(), window.end(), assignment), end; it != end;
	     ++it) {
		const size_t rhs = size_t(it->position() + it->length()) - 1;
		found = window.substr(rhs, window.find(';', rhs) - rhs);
	}
	return found;
}
}  // namespace wc_atomic_scan
