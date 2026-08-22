#pragma once

#include <cwctype>
#include <string_view>

namespace rex::equalizer::detail
{
inline constexpr std::wstring_view kManagedIncludeDirective = L"Include: rexs_toolkit_equalizer.txt";

inline std::wstring_view TrimConfigLine(std::wstring_view line)
{
    while (!line.empty() && std::iswspace(line.front())) line.remove_prefix(1);
    while (!line.empty() && std::iswspace(line.back())) line.remove_suffix(1);
    return line;
}

inline bool ConfigTextEquals(std::wstring_view left, std::wstring_view right)
{
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
    {
        if (std::towlower(left[index]) != std::towlower(right[index])) return false;
    }
    return true;
}

inline bool ContainsActiveManagedInclude(std::wstring_view text)
{
    size_t lineStart = 0;
    while (lineStart <= text.size())
    {
        const size_t lineEnd = text.find_first_of(L"\r\n", lineStart);
        const std::wstring_view line = TrimConfigLine(text.substr(
            lineStart,
            lineEnd == std::wstring_view::npos
                ? std::wstring_view::npos
                : lineEnd - lineStart));
        if (!line.empty() &&
            line.front() != L'#' &&
            line.front() != L';' &&
            ConfigTextEquals(line, kManagedIncludeDirective))
        {
            return true;
        }
        if (lineEnd == std::wstring_view::npos) break;
        lineStart = lineEnd + 1;
        while (lineStart < text.size() &&
            (text[lineStart] == L'\r' || text[lineStart] == L'\n'))
        {
            ++lineStart;
        }
    }
    return false;
}
}
