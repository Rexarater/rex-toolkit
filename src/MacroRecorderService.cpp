#include "MacroRecorderService.h"

#include <objbase.h>
#include <psapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <sstream>
#include <system_error>

namespace
{
constexpr ULONG_PTR kMacroPlaybackExtraInfo = 0x52584D43;
constexpr wchar_t kInterceptionDllRelativePath[] = L"tools\\interception\\x64\\interception.dll";

using InterceptionContext = void*;
using InterceptionDevice = int;
using InterceptionStroke = char[sizeof(int) * 5];

constexpr int kInterceptionMaxKeyboard = 10;
constexpr int kInterceptionMaxMouse = 10;
constexpr int kInterceptionKeyboardBase = 1;
constexpr int kInterceptionMouseBase = kInterceptionMaxKeyboard + 1;
constexpr unsigned short kInterceptionFilterAll = 0xFFFF;
constexpr unsigned short kInterceptionKeyUp = 0x01;
constexpr unsigned short kInterceptionKeyE0 = 0x02;
constexpr unsigned short kInterceptionKeyE1 = 0x04;
constexpr unsigned short kInterceptionMouseLeftDown = 0x001;
constexpr unsigned short kInterceptionMouseLeftUp = 0x002;
constexpr unsigned short kInterceptionMouseRightDown = 0x004;
constexpr unsigned short kInterceptionMouseRightUp = 0x008;
constexpr unsigned short kInterceptionMouseMiddleDown = 0x010;
constexpr unsigned short kInterceptionMouseMiddleUp = 0x020;
constexpr unsigned short kInterceptionMouseButton4Down = 0x040;
constexpr unsigned short kInterceptionMouseButton4Up = 0x080;
constexpr unsigned short kInterceptionMouseButton5Down = 0x100;
constexpr unsigned short kInterceptionMouseButton5Up = 0x200;
constexpr unsigned short kInterceptionMouseWheel = 0x400;
constexpr unsigned short kInterceptionMouseMoveRelative = 0x000;
constexpr unsigned short kInterceptionMouseMoveAbsolute = 0x001;
constexpr unsigned short kInterceptionMouseVirtualDesktop = 0x002;

struct InterceptionMouseStroke
{
    unsigned short state;
    unsigned short flags;
    short rolling;
    int x;
    int y;
    unsigned int information;
};

struct InterceptionKeyStroke
{
    unsigned short code;
    unsigned short state;
    unsigned int information;
};

MacroRecorderService* g_macroRecorder = nullptr;

std::wstring Utf8ToWide(const std::string& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0)
    {
        return {};
    }

    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(const std::wstring& text)
{
    if (text.empty())
    {
        return {};
    }

    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0)
    {
        return {};
    }

    std::string utf8(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

std::wstring Trim(std::wstring value)
{
    auto first = std::find_if_not(value.begin(), value.end(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    });
    auto last = std::find_if_not(value.rbegin(), value.rend(), [](wchar_t ch)
    {
        return iswspace(ch) != 0;
    }).base();
    if (first >= last)
    {
        return {};
    }
    return std::wstring(first, last);
}

std::string JsonEscape(const std::wstring& value)
{
    const std::string utf8 = WideToUtf8(value);
    std::string output;
    output.reserve(utf8.size() + 8);
    output.push_back('"');
    for (unsigned char ch : utf8)
    {
        switch (ch)
        {
        case '"':
            output += "\\\"";
            break;
        case '\\':
            output += "\\\\";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                std::ostringstream stream;
                stream << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
                output += stream.str();
            }
            else
            {
                output.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    output.push_back('"');
    return output;
}

std::wstring UnescapeJsonString(const std::string& text)
{
    std::string output;
    output.reserve(text.size());
    for (size_t index = 0; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (ch != '\\' || index + 1 >= text.size())
        {
            output.push_back(ch);
            continue;
        }

        const char escaped = text[++index];
        switch (escaped)
        {
        case '"':
        case '\\':
        case '/':
            output.push_back(escaped);
            break;
        case 'n':
            output.push_back('\n');
            break;
        case 'r':
            output.push_back('\r');
            break;
        case 't':
            output.push_back('\t');
            break;
        default:
            break;
        }
    }
    return Utf8ToWide(output);
}

size_t FindField(const std::string& object, const char* key)
{
    const std::string quotedKey = std::string("\"") + key + "\"";
    const size_t keyPosition = object.find(quotedKey);
    if (keyPosition == std::string::npos)
    {
        return std::string::npos;
    }
    const size_t colon = object.find(':', keyPosition + quotedKey.size());
    return colon == std::string::npos ? std::string::npos : colon + 1;
}

size_t SkipSpaces(const std::string& text, size_t position)
{
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position])) != 0)
    {
        ++position;
    }
    return position;
}

std::wstring ExtractString(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return {};
    }
    if (object.compare(position, 4, "null") == 0)
    {
        return {};
    }
    if (object[position] != '"')
    {
        return {};
    }

    ++position;
    std::string value;
    bool escaped = false;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
        if (escaped)
        {
            value.push_back('\\');
            value.push_back(ch);
            escaped = false;
            continue;
        }
        if (ch == '\\')
        {
            escaped = true;
            continue;
        }
        if (ch == '"')
        {
            break;
        }
        value.push_back(ch);
    }
    return UnescapeJsonString(value);
}

long long ExtractInt64(const std::string& object, const char* key, long long fallback = 0)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    return std::strtoll(object.c_str() + position, nullptr, 10);
}

int ExtractInt(const std::string& object, const char* key, int fallback = 0)
{
    return static_cast<int>(ExtractInt64(object, key, fallback));
}

double ExtractDouble(const std::string& object, const char* key, double fallback = 0.0)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    char* end = nullptr;
    const double value = std::strtod(object.c_str() + position, &end);
    return end == object.c_str() + position ? fallback : value;
}

bool ExtractBool(const std::string& object, const char* key, bool fallback = false)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size())
    {
        return fallback;
    }
    if (object.compare(position, 4, "true") == 0)
    {
        return true;
    }
    if (object.compare(position, 5, "false") == 0)
    {
        return false;
    }
    return fallback;
}

std::string ExtractObject(const std::string& object, const char* key)
{
    size_t position = SkipSpaces(object, FindField(object, key));
    if (position == std::string::npos || position >= object.size() || object[position] != '{')
    {
        return {};
    }

    const size_t objectStart = position;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (; position < object.size(); ++position)
    {
        const char ch = object[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0)
            {
                return object.substr(objectStart, position - objectStart + 1);
            }
        }
    }

    return {};
}

std::vector<std::string> ExtractObjectsInArray(const std::string& json, const char* key)
{
    std::vector<std::string> objects;
    size_t position = SkipSpaces(json, FindField(json, key));
    if (position == std::string::npos || position >= json.size() || json[position] != '[')
    {
        return objects;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;
    for (++position; position < json.size(); ++position)
    {
        const char ch = json[position];
        if (inString)
        {
            if (escaped)
            {
                escaped = false;
            }
            else if (ch == '\\')
            {
                escaped = true;
            }
            else if (ch == '"')
            {
                inString = false;
            }
            continue;
        }

        if (ch == '"')
        {
            inString = true;
        }
        else if (ch == '{')
        {
            if (depth == 0)
            {
                objectStart = position;
            }
            ++depth;
        }
        else if (ch == '}')
        {
            --depth;
            if (depth == 0 && objectStart != std::string::npos)
            {
                objects.push_back(json.substr(objectStart, position - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
        else if (ch == ']' && depth == 0)
        {
            break;
        }
    }
    return objects;
}

std::wstring EventTypeName(MacroEventType type)
{
    switch (type)
    {
    case MacroEventType::KeyDown:
        return L"keyDown";
    case MacroEventType::KeyUp:
        return L"keyUp";
    case MacroEventType::MouseDown:
        return L"mouseDown";
    case MacroEventType::MouseUp:
        return L"mouseUp";
    case MacroEventType::MouseMove:
        return L"mouseMove";
    case MacroEventType::MouseWheel:
        return L"mouseWheel";
    }
    return L"unknown";
}

MacroEventType EventTypeFromName(const std::wstring& name)
{
    if (name == L"keyUp") return MacroEventType::KeyUp;
    if (name == L"mouseDown") return MacroEventType::MouseDown;
    if (name == L"mouseUp") return MacroEventType::MouseUp;
    if (name == L"mouseMove") return MacroEventType::MouseMove;
    if (name == L"mouseWheel") return MacroEventType::MouseWheel;
    return MacroEventType::KeyDown;
}

std::wstring MouseButtonName(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return L"left";
    case MacroMouseButton::Right:
        return L"right";
    case MacroMouseButton::Middle:
        return L"middle";
    case MacroMouseButton::X1:
        return L"x1";
    case MacroMouseButton::X2:
        return L"x2";
    case MacroMouseButton::None:
        break;
    }
    return L"none";
}

MacroMouseButton MouseButtonFromName(const std::wstring& name)
{
    if (name == L"left") return MacroMouseButton::Left;
    if (name == L"right") return MacroMouseButton::Right;
    if (name == L"middle") return MacroMouseButton::Middle;
    if (name == L"x1") return MacroMouseButton::X1;
    if (name == L"x2") return MacroMouseButton::X2;
    return MacroMouseButton::None;
}

std::wstring MouseModeName(MacroMouseMode mode)
{
    switch (mode)
    {
    case MacroMouseMode::Absolute:
        return L"Absolute";
    case MacroMouseMode::WindowRelative:
        return L"WindowRelative";
    case MacroMouseMode::Relative:
        break;
    }
    return L"Relative";
}

MacroMouseMode MouseModeFromName(const std::wstring& name)
{
    if (name == L"Absolute") return MacroMouseMode::Absolute;
    if (name == L"WindowRelative") return MacroMouseMode::WindowRelative;
    return MacroMouseMode::Relative;
}

DWORD MouseDownFlag(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return MOUSEEVENTF_LEFTDOWN;
    case MacroMouseButton::Right:
        return MOUSEEVENTF_RIGHTDOWN;
    case MacroMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEDOWN;
    case MacroMouseButton::X1:
    case MacroMouseButton::X2:
        return MOUSEEVENTF_XDOWN;
    case MacroMouseButton::None:
        break;
    }
    return 0;
}

DWORD MouseUpFlag(MacroMouseButton button)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return MOUSEEVENTF_LEFTUP;
    case MacroMouseButton::Right:
        return MOUSEEVENTF_RIGHTUP;
    case MacroMouseButton::Middle:
        return MOUSEEVENTF_MIDDLEUP;
    case MacroMouseButton::X1:
    case MacroMouseButton::X2:
        return MOUSEEVENTF_XUP;
    case MacroMouseButton::None:
        break;
    }
    return 0;
}

DWORD XButtonData(MacroMouseButton button)
{
    if (button == MacroMouseButton::X1)
    {
        return XBUTTON1;
    }
    if (button == MacroMouseButton::X2)
    {
        return XBUTTON2;
    }
    return 0;
}

MacroMouseButton MouseButtonFromMessage(WPARAM message, const MSLLHOOKSTRUCT& mouse)
{
    switch (message)
    {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return MacroMouseButton::Left;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return MacroMouseButton::Right;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return MacroMouseButton::Middle;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        return HIWORD(mouse.mouseData) == XBUTTON2 ? MacroMouseButton::X2 : MacroMouseButton::X1;
    default:
        return MacroMouseButton::None;
    }
}

bool IsMouseDownMessage(WPARAM message)
{
    return message == WM_LBUTTONDOWN ||
        message == WM_RBUTTONDOWN ||
        message == WM_MBUTTONDOWN ||
        message == WM_XBUTTONDOWN;
}

bool IsMouseUpMessage(WPARAM message)
{
    return message == WM_LBUTTONUP ||
        message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP ||
        message == WM_XBUTTONUP;
}

void WriteStringField(std::ostringstream& output, const char* indent, const char* key, const std::wstring& value, bool comma = true)
{
    output << indent << "\"" << key << "\": " << JsonEscape(value);
    if (comma)
    {
        output << ",";
    }
    output << "\n";
}

std::filesystem::path MacroFilePath(const std::filesystem::path& directory, const MacroDefinition& macro)
{
    const std::wstring id = macro.id.empty() ? MacroStorageService::GenerateId() : macro.id;
    return directory / (id + L".rexmacro");
}

bool ReadFileBytes(const std::filesystem::path& path, std::string& bytes)
{
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
    {
        bytes.erase(0, 3);
    }
    return true;
}

MacroEvent EventFromJson(const std::string& object)
{
    MacroEvent event;
    event.timeUs = ExtractInt64(object, "timeUs", ExtractInt64(object, "timeMs", 0) * 1000);
    event.type = EventTypeFromName(ExtractString(object, "type"));
    event.virtualKey = static_cast<DWORD>(ExtractInt(object, "keyCode"));
    event.scanCode = static_cast<DWORD>(ExtractInt(object, "scanCode"));
    event.keyFlags = static_cast<DWORD>(ExtractInt(object, "keyFlags"));
    event.mouseButton = MouseButtonFromName(ExtractString(object, "mouseButton"));
    event.x = static_cast<LONG>(ExtractInt(object, "x"));
    event.y = static_cast<LONG>(ExtractInt(object, "y"));
    event.dx = static_cast<LONG>(ExtractInt(object, "dx"));
    event.dy = static_cast<LONG>(ExtractInt(object, "dy"));
    event.wheelDelta = ExtractInt(object, "wheelDelta");
    event.absoluteMove = ExtractBool(object, "absoluteMove");
    event.shift = ExtractBool(object, "shift");
    event.ctrl = ExtractBool(object, "ctrl");
    event.alt = ExtractBool(object, "alt");
    return event;
}

std::optional<MacroDefinition> MacroFromJson(const std::string& json)
{
    if (json.find("\"events\"") == std::string::npos)
    {
        return std::nullopt;
    }

    MacroDefinition macro;
    macro.version = ExtractInt(json, "version", 1);
    macro.id = ExtractString(json, "id");
    macro.name = ExtractString(json, "name");
    macro.description = ExtractString(json, "description");
    macro.createdAt = ExtractString(json, "createdAt");
    macro.updatedAt = ExtractString(json, "updatedAt");
    macro.targetWindowTitle = ExtractString(json, "targetWindowTitle");
    macro.targetProcessName = ExtractString(json, "targetProcessName");
    macro.defaultPlaybackSpeed = std::clamp(ExtractDouble(json, "defaultPlaybackSpeed", 1.0), 0.1, 10.0);
    macro.defaultLoopCount = std::clamp(ExtractInt(json, "defaultLoopCount", 1), 1, 999);
    macro.defaultLoopUntilStopped = ExtractBool(json, "defaultLoopUntilStopped");
    macro.requireTargetFocused = ExtractBool(json, "requireTargetFocused");

    const std::string modeObject = ExtractObject(json, "recordingMode");
    if (!modeObject.empty())
    {
        macro.recordingMode.mouseMode = MouseModeFromName(ExtractString(modeObject, "mouseMode"));
        macro.recordingMode.captureRateHz = std::clamp(ExtractInt(modeObject, "captureRateHz", 120), 30, 240);
    }

    for (const std::string& object : ExtractObjectsInArray(json, "events"))
    {
        MacroEvent event = EventFromJson(object);
        if (event.timeUs >= 0)
        {
            macro.events.push_back(event);
        }
    }

    std::sort(macro.events.begin(), macro.events.end(), [](const MacroEvent& left, const MacroEvent& right)
    {
        return left.timeUs < right.timeUs;
    });

    if (macro.id.empty())
    {
        macro.id = MacroStorageService::GenerateId();
    }
    if (Trim(macro.name).empty())
    {
        macro.name = L"Imported Macro";
    }
    if (macro.createdAt.empty())
    {
        macro.createdAt = MacroStorageService::FormatLocalIso(std::chrono::system_clock::now());
    }
    if (macro.updatedAt.empty())
    {
        macro.updatedAt = macro.createdAt;
    }
    if (macro.events.empty())
    {
        return std::nullopt;
    }

    return macro;
}

std::string MacroToJson(const MacroDefinition& macro)
{
    std::ostringstream output;
    output << "{\n";
    output << "  \"version\": 1,\n";
    WriteStringField(output, "  ", "id", macro.id);
    WriteStringField(output, "  ", "name", macro.name);
    WriteStringField(output, "  ", "description", macro.description);
    WriteStringField(output, "  ", "createdAt", macro.createdAt);
    WriteStringField(output, "  ", "updatedAt", macro.updatedAt);
    WriteStringField(output, "  ", "targetWindowTitle", macro.targetWindowTitle);
    WriteStringField(output, "  ", "targetProcessName", macro.targetProcessName);
    output << "  \"recordingMode\": {\n";
    WriteStringField(output, "    ", "mouseMode", MouseModeName(macro.recordingMode.mouseMode));
    output << "    \"captureRateHz\": " << macro.recordingMode.captureRateHz << "\n";
    output << "  },\n";
    output << "  \"defaultPlaybackSpeed\": " << std::fixed << std::setprecision(2) << macro.defaultPlaybackSpeed << ",\n";
    output << "  \"defaultLoopCount\": " << macro.defaultLoopCount << ",\n";
    output << "  \"defaultLoopUntilStopped\": " << (macro.defaultLoopUntilStopped ? "true" : "false") << ",\n";
    output << "  \"requireTargetFocused\": " << (macro.requireTargetFocused ? "true" : "false") << ",\n";
    output << "  \"events\": [\n";
    for (size_t index = 0; index < macro.events.size(); ++index)
    {
        const MacroEvent& event = macro.events[index];
        output << "    {\n";
        output << "      \"timeUs\": " << event.timeUs << ",\n";
        WriteStringField(output, "      ", "type", EventTypeName(event.type));
        output << "      \"keyCode\": " << event.virtualKey << ",\n";
        output << "      \"scanCode\": " << event.scanCode << ",\n";
        output << "      \"keyFlags\": " << event.keyFlags << ",\n";
        WriteStringField(output, "      ", "mouseButton", MouseButtonName(event.mouseButton));
        output << "      \"x\": " << event.x << ",\n";
        output << "      \"y\": " << event.y << ",\n";
        output << "      \"dx\": " << event.dx << ",\n";
        output << "      \"dy\": " << event.dy << ",\n";
        output << "      \"wheelDelta\": " << event.wheelDelta << ",\n";
        output << "      \"absoluteMove\": " << (event.absoluteMove ? "true" : "false") << ",\n";
        output << "      \"shift\": " << (event.shift ? "true" : "false") << ",\n";
        output << "      \"ctrl\": " << (event.ctrl ? "true" : "false") << ",\n";
        output << "      \"alt\": " << (event.alt ? "true" : "false") << "\n";
        output << "    }" << (index + 1 == macro.events.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
    return output.str();
}

std::wstring ProcessNameFromId(DWORD processId)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process)
    {
        return {};
    }

    wchar_t path[MAX_PATH] {};
    DWORD length = static_cast<DWORD>(std::size(path));
    std::wstring result;
    if (QueryFullProcessImageNameW(process, 0, path, &length) && length > 0)
    {
        result = std::filesystem::path(path).filename().wstring();
    }
    CloseHandle(process);
    return result;
}

std::wstring KeyName(UINT virtualKey)
{
    if (virtualKey == 0)
    {
        return L"Unbound";
    }

    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    LONG lParam = static_cast<LONG>(scanCode << 16);
    if (virtualKey == VK_LEFT || virtualKey == VK_UP || virtualKey == VK_RIGHT || virtualKey == VK_DOWN ||
        virtualKey == VK_PRIOR || virtualKey == VK_NEXT || virtualKey == VK_END || virtualKey == VK_HOME ||
        virtualKey == VK_INSERT || virtualKey == VK_DELETE || virtualKey == VK_DIVIDE || virtualKey == VK_NUMLOCK)
    {
        lParam |= (1 << 24);
    }

    wchar_t name[64] {};
    if (GetKeyNameTextW(lParam, name, static_cast<int>(std::size(name))) > 0)
    {
        return name;
    }

    if (virtualKey >= 'A' && virtualKey <= 'Z')
    {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    if (virtualKey >= '0' && virtualKey <= '9')
    {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }

    return L"VK " + std::to_wstring(virtualKey);
}

LONG NormalizeAbsoluteCoordinate(LONG value, int origin, int size)
{
    if (size <= 1)
    {
        return 0;
    }
    return static_cast<LONG>(std::clamp(
        static_cast<long long>(value - origin) * 65535ll / static_cast<long long>(size - 1),
        0ll,
        65535ll));
}

bool CurrentModifiersMatch(UINT modifiers)
{
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (((modifiers & MOD_SHIFT) != 0) != shift) return false;
    if (((modifiers & MOD_CONTROL) != 0) != ctrl) return false;
    if (((modifiers & MOD_ALT) != 0) != alt) return false;
    return true;
}

bool IsCursorShowing()
{
    CURSORINFO cursorInfo {};
    cursorInfo.cbSize = sizeof(cursorInfo);
    return GetCursorInfo(&cursorInfo) &&
        (cursorInfo.flags & CURSOR_SHOWING) != 0;
}

std::filesystem::path ExecutableDirectory()
{
    std::vector<wchar_t> buffer(MAX_PATH, L'\0');
    while (true)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0)
        {
            return {};
        }
        if (length < buffer.size() - 1)
        {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2, L'\0');
    }
}

unsigned short InterceptionMouseStateForButton(MacroMouseButton button, bool down)
{
    switch (button)
    {
    case MacroMouseButton::Left:
        return down ? kInterceptionMouseLeftDown : kInterceptionMouseLeftUp;
    case MacroMouseButton::Right:
        return down ? kInterceptionMouseRightDown : kInterceptionMouseRightUp;
    case MacroMouseButton::Middle:
        return down ? kInterceptionMouseMiddleDown : kInterceptionMouseMiddleUp;
    case MacroMouseButton::X1:
        return down ? kInterceptionMouseButton4Down : kInterceptionMouseButton4Up;
    case MacroMouseButton::X2:
        return down ? kInterceptionMouseButton5Down : kInterceptionMouseButton5Up;
    case MacroMouseButton::None:
        break;
    }
    return 0;
}

class InterceptionPlaybackBackend
{
public:
    ~InterceptionPlaybackBackend()
    {
        if (context_ && destroyContext_)
        {
            destroyContext_(context_);
        }
        if (module_)
        {
            FreeLibrary(module_);
        }
    }

    bool Initialize(std::wstring& errorMessage)
    {
        const std::filesystem::path bundledPath = ExecutableDirectory() / kInterceptionDllRelativePath;
        module_ = LoadLibraryW(bundledPath.c_str());
        if (!module_)
        {
            module_ = LoadLibraryW(L"interception.dll");
        }
        if (!module_)
        {
            errorMessage = L"Interception playback is unavailable. The bundled interception.dll was not found beside Rex's Toolkit.";
            return false;
        }

        createContext_ = reinterpret_cast<CreateContextProc>(GetProcAddress(module_, "interception_create_context"));
        destroyContext_ = reinterpret_cast<DestroyContextProc>(GetProcAddress(module_, "interception_destroy_context"));
        send_ = reinterpret_cast<SendProc>(GetProcAddress(module_, "interception_send"));
        getHardwareId_ = reinterpret_cast<GetHardwareIdProc>(GetProcAddress(module_, "interception_get_hardware_id"));
        if (!createContext_ || !destroyContext_ || !send_ || !getHardwareId_)
        {
            errorMessage = L"Interception playback is unavailable. The bundled interception.dll is missing required functions.";
            return false;
        }

        context_ = createContext_();
        if (!context_)
        {
            errorMessage = L"Interception playback is unavailable. Install the bundled Interception driver as administrator, then restart Rex's Toolkit.";
            return false;
        }

        keyboardDevice_ = FindDevice(kInterceptionKeyboardBase, kInterceptionMaxKeyboard);
        mouseDevice_ = FindDevice(kInterceptionMouseBase, kInterceptionMaxMouse);
        if (keyboardDevice_ <= 0 && mouseDevice_ <= 0)
        {
            errorMessage = L"Interception playback is unavailable. No Interception keyboard or mouse devices were found. Install the driver as administrator, then restart Rex's Toolkit.";
            return false;
        }

        return true;
    }

    bool SendEvent(const MacroEvent& event, MacroMouseMode mode)
    {
        if (event.type == MacroEventType::KeyDown || event.type == MacroEventType::KeyUp)
        {
            if (keyboardDevice_ <= 0)
            {
                return false;
            }

            InterceptionKeyStroke key {};
            key.code = static_cast<unsigned short>(event.scanCode != 0
                ? event.scanCode
                : MapVirtualKeyW(event.virtualKey, MAPVK_VK_TO_VSC));
            if (key.code == 0)
            {
                return false;
            }
            key.state = (event.type == MacroEventType::KeyUp ? kInterceptionKeyUp : 0);
            if ((event.keyFlags & LLKHF_EXTENDED) != 0)
            {
                key.state |= kInterceptionKeyE0;
            }
            key.information = static_cast<unsigned int>(kMacroPlaybackExtraInfo);

            InterceptionStroke stroke {};
            std::memcpy(stroke, &key, sizeof(key));
            return send_(context_, keyboardDevice_, &stroke, 1) == 1;
        }

        if (mouseDevice_ <= 0)
        {
            return false;
        }

        InterceptionMouseStroke mouse {};
        mouse.information = static_cast<unsigned int>(kMacroPlaybackExtraInfo);
        if (event.type == MacroEventType::MouseMove)
        {
            if (mode == MacroMouseMode::Relative && !event.absoluteMove)
            {
                mouse.flags = kInterceptionMouseMoveRelative;
                mouse.x = event.dx;
                mouse.y = event.dy;
            }
            else
            {
                LONG x = event.x;
                LONG y = event.y;
                if (!event.absoluteMove && mode == MacroMouseMode::WindowRelative)
                {
                    HWND foreground = GetForegroundWindow();
                    RECT rect {};
                    if (foreground && GetWindowRect(foreground, &rect))
                    {
                        x += rect.left;
                        y += rect.top;
                    }
                }

                const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                mouse.flags = kInterceptionMouseMoveAbsolute | kInterceptionMouseVirtualDesktop;
                mouse.x = NormalizeAbsoluteCoordinate(x, originX, width);
                mouse.y = NormalizeAbsoluteCoordinate(y, originY, height);
            }
        }
        else if (event.type == MacroEventType::MouseDown || event.type == MacroEventType::MouseUp)
        {
            mouse.state = InterceptionMouseStateForButton(event.mouseButton, event.type == MacroEventType::MouseDown);
        }
        else if (event.type == MacroEventType::MouseWheel)
        {
            mouse.state = kInterceptionMouseWheel;
            mouse.rolling = static_cast<short>(event.wheelDelta);
        }

        if (mouse.state == 0 && mouse.flags == 0 && event.type != MacroEventType::MouseMove)
        {
            return false;
        }

        InterceptionStroke stroke {};
        std::memcpy(stroke, &mouse, sizeof(mouse));
        return send_(context_, mouseDevice_, &stroke, 1) == 1;
    }

private:
    using CreateContextProc = InterceptionContext (*)();
    using DestroyContextProc = void (*)(InterceptionContext);
    using SendProc = int (*)(InterceptionContext, InterceptionDevice, const InterceptionStroke*, unsigned int);
    using GetHardwareIdProc = unsigned int (*)(InterceptionContext, InterceptionDevice, void*, unsigned int);

    InterceptionDevice FindDevice(int base, int count) const
    {
        wchar_t hardwareId[512] {};
        for (int index = 0; index < count; ++index)
        {
            const InterceptionDevice device = base + index;
            std::memset(hardwareId, 0, sizeof(hardwareId));
            if (getHardwareId_(context_, device, hardwareId, sizeof(hardwareId)) > 0)
            {
                return device;
            }
        }
        return 0;
    }

    HMODULE module_ = nullptr;
    InterceptionContext context_ = nullptr;
    InterceptionDevice keyboardDevice_ = 0;
    InterceptionDevice mouseDevice_ = 0;
    CreateContextProc createContext_ = nullptr;
    DestroyContextProc destroyContext_ = nullptr;
    SendProc send_ = nullptr;
    GetHardwareIdProc getHardwareId_ = nullptr;
};
}

class InterceptionCaptureSession
{
public:
    explicit InterceptionCaptureSession(MacroRecorderService& owner) : owner_(owner) {}

    ~InterceptionCaptureSession()
    {
        Stop();
        if (context_ && destroyContext_)
        {
            destroyContext_(context_);
        }
        if (module_)
        {
            FreeLibrary(module_);
        }
    }

    bool Initialize(std::wstring& errorMessage)
    {
        const std::filesystem::path bundledPath = ExecutableDirectory() / kInterceptionDllRelativePath;
        module_ = LoadLibraryW(bundledPath.c_str());
        if (!module_)
        {
            module_ = LoadLibraryW(L"interception.dll");
        }
        if (!module_)
        {
            errorMessage = L"Driver-enhanced recording is unavailable. The bundled interception.dll was not found beside Rex's Toolkit.";
            return false;
        }

        createContext_ = reinterpret_cast<CreateContextProc>(GetProcAddress(module_, "interception_create_context"));
        destroyContext_ = reinterpret_cast<DestroyContextProc>(GetProcAddress(module_, "interception_destroy_context"));
        setFilter_ = reinterpret_cast<SetFilterProc>(GetProcAddress(module_, "interception_set_filter"));
        waitWithTimeout_ = reinterpret_cast<WaitWithTimeoutProc>(GetProcAddress(module_, "interception_wait_with_timeout"));
        receive_ = reinterpret_cast<ReceiveProc>(GetProcAddress(module_, "interception_receive"));
        send_ = reinterpret_cast<SendProc>(GetProcAddress(module_, "interception_send"));
        isKeyboard_ = reinterpret_cast<PredicateProc>(GetProcAddress(module_, "interception_is_keyboard"));
        isMouse_ = reinterpret_cast<PredicateProc>(GetProcAddress(module_, "interception_is_mouse"));
        if (!createContext_ || !destroyContext_ || !setFilter_ || !waitWithTimeout_ || !receive_ || !send_ || !isKeyboard_ || !isMouse_)
        {
            errorMessage = L"Driver-enhanced recording is unavailable. The bundled interception.dll is missing required functions.";
            return false;
        }

        context_ = createContext_();
        if (!context_)
        {
            errorMessage = L"Driver-enhanced recording is unavailable. Install the bundled Interception driver as administrator, then restart Windows.";
            return false;
        }

        return true;
    }

    bool Start(std::wstring& errorMessage)
    {
        if (!context_ || !setFilter_ || !waitWithTimeout_ || !receive_ || !send_)
        {
            errorMessage = L"Driver-enhanced recording is unavailable.";
            return false;
        }

        stopRequested_.store(false);
        setFilter_(context_, isKeyboard_, kInterceptionFilterAll);
        setFilter_(context_, isMouse_, kInterceptionFilterAll);

        try
        {
            captureThread_ = std::thread(&InterceptionCaptureSession::CaptureLoop, this);
        }
        catch (const std::system_error&)
        {
            setFilter_(context_, isKeyboard_, 0);
            setFilter_(context_, isMouse_, 0);
            stopRequested_.store(true);
            errorMessage = L"Driver-enhanced recording could not start its capture thread.";
            return false;
        }
        return true;
    }

    void Stop()
    {
        stopRequested_.store(true);
        if (context_ && setFilter_)
        {
            setFilter_(context_, isKeyboard_, 0);
            setFilter_(context_, isMouse_, 0);
        }
        if (captureThread_.joinable())
        {
            captureThread_.join();
        }
    }

private:
    using PredicateProc = int (*)(InterceptionDevice);
    using CreateContextProc = InterceptionContext (*)();
    using DestroyContextProc = void (*)(InterceptionContext);
    using SetFilterProc = void (*)(InterceptionContext, PredicateProc, unsigned short);
    using WaitWithTimeoutProc = InterceptionDevice (*)(InterceptionContext, unsigned long);
    using ReceiveProc = int (*)(InterceptionContext, InterceptionDevice, InterceptionStroke*, unsigned int);
    using SendProc = int (*)(InterceptionContext, InterceptionDevice, const InterceptionStroke*, unsigned int);

    void CaptureLoop()
    {
        const bool timerResolutionRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
        const HANDLE currentThread = GetCurrentThread();
        const int previousThreadPriority = GetThreadPriority(currentThread);
        if (previousThreadPriority != THREAD_PRIORITY_ERROR_RETURN)
        {
            SetThreadPriority(currentThread, THREAD_PRIORITY_TIME_CRITICAL);
        }

        while (!stopRequested_.load())
        {
            const InterceptionDevice device = waitWithTimeout_(context_, 5);
            if (device <= 0)
            {
                continue;
            }

            InterceptionStroke stroke {};
            if (receive_(context_, device, &stroke, 1) <= 0)
            {
                continue;
            }

            const long long eventTimeUs = owner_.ElapsedUs();

            // Pass the real hardware event through immediately; recording work happens after that.
            send_(context_, device, &stroke, 1);

            if (isKeyboard_(device))
            {
                InterceptionKeyStroke key {};
                std::memcpy(&key, &stroke, sizeof(key));
                owner_.RecordInterceptionKeyboard(key.code, key.state, key.information, eventTimeUs);
            }
            else if (isMouse_(device))
            {
                InterceptionMouseStroke mouse {};
                std::memcpy(&mouse, &stroke, sizeof(mouse));
                owner_.RecordInterceptionMouse(mouse.state, mouse.flags, mouse.rolling, mouse.x, mouse.y, mouse.information, eventTimeUs);
            }
        }

        if (previousThreadPriority != THREAD_PRIORITY_ERROR_RETURN)
        {
            SetThreadPriority(currentThread, previousThreadPriority);
        }
        if (timerResolutionRaised)
        {
            timeEndPeriod(1);
        }
    }

    MacroRecorderService& owner_;
    HMODULE module_ = nullptr;
    InterceptionContext context_ = nullptr;
    std::thread captureThread_;
    std::atomic_bool stopRequested_ = true;
    CreateContextProc createContext_ = nullptr;
    DestroyContextProc destroyContext_ = nullptr;
    SetFilterProc setFilter_ = nullptr;
    WaitWithTimeoutProc waitWithTimeout_ = nullptr;
    ReceiveProc receive_ = nullptr;
    SendProc send_ = nullptr;
    PredicateProc isKeyboard_ = nullptr;
    PredicateProc isMouse_ = nullptr;
};

void MacroTargetWindowService::CaptureForegroundTarget(std::wstring& title, std::wstring& processName)
{
    title.clear();
    processName.clear();

    HWND foreground = GetForegroundWindow();
    if (!foreground)
    {
        return;
    }

    wchar_t titleBuffer[256] {};
    GetWindowTextW(foreground, titleBuffer, static_cast<int>(std::size(titleBuffer)));
    title = titleBuffer;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId != 0)
    {
        processName = ProcessNameFromId(processId);
    }
}

bool MacroTargetWindowService::IsTargetFocused(const MacroDefinition& macro)
{
    if (macro.targetWindowTitle.empty() && macro.targetProcessName.empty())
    {
        return true;
    }

    std::wstring title;
    std::wstring processName;
    CaptureForegroundTarget(title, processName);
    if (!macro.targetProcessName.empty() && processName == macro.targetProcessName)
    {
        return true;
    }
    return !macro.targetWindowTitle.empty() && title == macro.targetWindowTitle;
}

std::wstring MacroTargetWindowService::ForegroundWindowLabel()
{
    std::wstring title;
    std::wstring processName;
    CaptureForegroundTarget(title, processName);
    if (title.empty() && processName.empty())
    {
        return L"No foreground window detected";
    }
    if (processName.empty())
    {
        return title;
    }
    if (title.empty())
    {
        return processName;
    }
    return title + L" (" + processName + L")";
}

std::wstring MacroStorageService::GenerateId()
{
    GUID guid {};
    if (FAILED(CoCreateGuid(&guid)))
    {
        return L"macro_" + std::to_wstring(GetTickCount64());
    }

    wchar_t buffer[64] {};
    StringFromGUID2(guid, buffer, static_cast<int>(std::size(buffer)));
    std::wstring text = buffer;
    text.erase(std::remove(text.begin(), text.end(), L'{'), text.end());
    text.erase(std::remove(text.begin(), text.end(), L'}'), text.end());
    return text;
}

std::wstring MacroStorageService::FormatLocalIso(std::chrono::system_clock::time_point value)
{
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(value);
    std::tm local {};
    localtime_s(&local, &timeValue);
    wchar_t buffer[32] {};
    wcsftime(buffer, std::size(buffer), L"%Y-%m-%dT%H:%M:%S", &local);
    return buffer;
}

std::vector<MacroDefinition> MacroStorageService::LoadAll(const std::filesystem::path& directory, std::wstring& warning) const
{
    std::vector<MacroDefinition> macros;
    std::error_code error;
    if (!std::filesystem::exists(directory, error))
    {
        return macros;
    }

    for (const auto& entry : std::filesystem::directory_iterator(directory, error))
    {
        if (error || !entry.is_regular_file())
        {
            continue;
        }

        const std::wstring extension = entry.path().extension().wstring();
        if (extension != L".rexmacro" && extension != L".json")
        {
            continue;
        }

        std::string json;
        if (!ReadFileBytes(entry.path(), json))
        {
            warning = L"Some macros could not be read and were skipped.";
            continue;
        }

        auto macro = MacroFromJson(json);
        if (macro)
        {
            macros.push_back(std::move(*macro));
        }
        else
        {
            warning = L"Some macros were invalid and were skipped.";
        }
    }

    std::sort(macros.begin(), macros.end(), [](const MacroDefinition& left, const MacroDefinition& right)
    {
        return left.updatedAt > right.updatedAt;
    });
    return macros;
}

bool MacroStorageService::Save(const std::filesystem::path& directory, const MacroDefinition& macro, std::wstring& errorMessage) const
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error)
    {
        errorMessage = L"Could not create the macro storage folder.";
        return false;
    }

    const std::filesystem::path path = MacroFilePath(directory, macro);
    if (std::filesystem::exists(path, error))
    {
        std::filesystem::copy_file(path, path.wstring() + L".bak", std::filesystem::copy_options::overwrite_existing, error);
        error.clear();
    }

    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = L"Could not save the macro file.";
        return false;
    }

    const std::string json = MacroToJson(macro);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    return file.good();
}

bool MacroStorageService::Delete(const std::filesystem::path& directory, const MacroDefinition& macro, std::wstring& errorMessage) const
{
    std::error_code error;
    std::filesystem::remove(MacroFilePath(directory, macro), error);
    if (error)
    {
        errorMessage = L"Could not delete the macro file.";
        return false;
    }
    return true;
}

bool MacroStorageService::ExportMacro(const MacroDefinition& macro, const std::filesystem::path& path, std::wstring& errorMessage) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file)
    {
        errorMessage = L"Could not export the macro.";
        return false;
    }

    const std::string json = MacroToJson(macro);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    return file.good();
}

std::optional<MacroDefinition> MacroStorageService::ImportMacro(const std::filesystem::path& path, std::wstring& errorMessage) const
{
    std::string json;
    if (!ReadFileBytes(path, json))
    {
        errorMessage = L"Could not read the selected macro file.";
        return std::nullopt;
    }

    auto macro = MacroFromJson(json);
    if (!macro)
    {
        errorMessage = L"That file is not a valid Rex macro.";
        return std::nullopt;
    }

    macro->id = GenerateId();
    macro->createdAt = FormatLocalIso(std::chrono::system_clock::now());
    macro->updatedAt = macro->createdAt;
    return macro;
}

MacroRecorderService::MacroRecorderService() = default;

MacroRecorderService::~MacroRecorderService()
{
    CancelRecording();
}

bool MacroRecorderService::StartRecording(
    HINSTANCE instance,
    HWND rawInputWindow,
    const MacroRecordingMode& mode,
    MacroPlaybackBackend inputBackend,
    const std::vector<MacroHotkey>& ignoredHotkeys,
    std::wstring& errorMessage)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == MacroRecorderStatus::Recording)
    {
        errorMessage = L"Recording is already running.";
        return false;
    }

    events_.clear();
    events_.reserve(65536);
    mode_ = mode;
    mode_.captureRateHz = std::clamp(mode_.captureRateHz, 30, 240);
    ignoredHotkeys_ = ignoredHotkeys;
    haveLastMousePoint_ = false;
    haveLastAbsoluteMousePoint_ = false;
    lastMouseMoveUs_ = 0;
    lastAbsoluteMouseMoveUs_ = 0;
    pendingMouseStartUs_ = 0;
    pendingMouseDx_ = 0;
    pendingMouseDy_ = 0;
    rawInputWindow_ = rawInputWindow;
    rawMouseRegistered_ = false;
    rawKeyboardRegistered_ = false;
    interceptionRecording_ = false;
    interceptionCaptureSession_.reset();
    rawKeyDown_.clear();
    MacroTargetWindowService::CaptureForegroundTarget(targetWindowTitle_, targetProcessName_);
    startTime_ = std::chrono::steady_clock::now();
    lastEventTime_ = startTime_;
    startTickMs_ = GetTickCount();

    if (inputBackend == MacroPlaybackBackend::Interception)
    {
        auto session = std::make_unique<InterceptionCaptureSession>(*this);
        if (!session->Initialize(errorMessage))
        {
            status_ = MacroRecorderStatus::Idle;
            return false;
        }

        status_ = MacroRecorderStatus::Recording;
        interceptionRecording_ = true;
        interceptionCaptureSession_ = std::move(session);
        if (!interceptionCaptureSession_->Start(errorMessage))
        {
            interceptionCaptureSession_.reset();
            interceptionRecording_ = false;
            status_ = MacroRecorderStatus::Idle;
            return false;
        }
        return true;
    }

    g_macroRecorder = this;
    if (mode_.mouseMode == MacroMouseMode::Relative)
    {
        rawMouseRegistered_ = RegisterRawMouseInput(rawInputWindow_);
        rawKeyboardRegistered_ = RegisterRawKeyboardInput(rawInputWindow_);
    }

    const bool needsKeyboardHook = !rawKeyboardRegistered_;
    const bool needsMouseHook = mode_.mouseMode != MacroMouseMode::Relative || !rawMouseRegistered_;
    if (needsKeyboardHook)
    {
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, MacroRecorderService::KeyboardHookProc, instance, 0);
    }
    if (needsMouseHook)
    {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, MacroRecorderService::MouseHookProc, instance, 0);
    }

    if ((needsKeyboardHook && !keyboardHook_) || (needsMouseHook && !mouseHook_))
    {
        if (keyboardHook_)
        {
            UnhookWindowsHookEx(keyboardHook_);
            keyboardHook_ = nullptr;
        }
        if (mouseHook_)
        {
            UnhookWindowsHookEx(mouseHook_);
            mouseHook_ = nullptr;
        }
        UnregisterRawMouseInput();
        UnregisterRawKeyboardInput();
        g_macroRecorder = nullptr;
        status_ = MacroRecorderStatus::Idle;
        errorMessage = L"Could not start recording. Windows did not allow the input hooks.";
        return false;
    }

    status_ = MacroRecorderStatus::Recording;
    return true;
}

MacroDefinition MacroRecorderService::StopRecording(const std::wstring& requestedName)
{
    std::unique_ptr<InterceptionCaptureSession> captureSession;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captureSession = std::move(interceptionCaptureSession_);
    }
    if (captureSession)
    {
        captureSession->Stop();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_.mouseMode == MacroMouseMode::Relative && (pendingMouseDx_ != 0 || pendingMouseDy_ != 0))
    {
        MacroEvent event;
        event.timeUs = ElapsedUs();
        event.type = MacroEventType::MouseMove;
        event.dx = pendingMouseDx_;
        event.dy = pendingMouseDy_;
        events_.push_back(event);
        pendingMouseDx_ = 0;
        pendingMouseDy_ = 0;
        pendingMouseStartUs_ = 0;
    }
    if (keyboardHook_)
    {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_)
    {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    UnregisterRawMouseInput();
    UnregisterRawKeyboardInput();
    g_macroRecorder = nullptr;
    interceptionRecording_ = false;
    status_ = MacroRecorderStatus::Stopped;

    MacroDefinition macro;
    macro.id = MacroStorageService::GenerateId();
    macro.name = Trim(requestedName).empty() ? L"New Macro" : Trim(requestedName);
    macro.createdAt = MacroStorageService::FormatLocalIso(std::chrono::system_clock::now());
    macro.updatedAt = macro.createdAt;
    macro.targetWindowTitle = targetWindowTitle_;
    macro.targetProcessName = targetProcessName_;
    macro.recordingMode = mode_;
    macro.events = events_;
    std::stable_sort(macro.events.begin(), macro.events.end(), [](const MacroEvent& left, const MacroEvent& right)
    {
        return left.timeUs < right.timeUs;
    });
    return macro;
}

void MacroRecorderService::CancelRecording()
{
    std::unique_ptr<InterceptionCaptureSession> captureSession;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        captureSession = std::move(interceptionCaptureSession_);
    }
    if (captureSession)
    {
        captureSession->Stop();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (keyboardHook_)
    {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (mouseHook_)
    {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    UnregisterRawMouseInput();
    UnregisterRawKeyboardInput();
    if (g_macroRecorder == this)
    {
        g_macroRecorder = nullptr;
    }
    status_ = MacroRecorderStatus::Idle;
    interceptionRecording_ = false;
    events_.clear();
    rawKeyDown_.clear();
    pendingMouseDx_ = 0;
    pendingMouseDy_ = 0;
    pendingMouseStartUs_ = 0;
    haveLastAbsoluteMousePoint_ = false;
    lastAbsoluteMouseMoveUs_ = 0;
}

bool MacroRecorderService::HandleRawInput(LPARAM lParam)
{
    if (mode_.mouseMode != MacroMouseMode::Relative || status_ != MacroRecorderStatus::Recording)
    {
        return false;
    }

    RAWINPUT stackInput {};
    UINT size = sizeof(stackInput);
    UINT bytesRead = GetRawInputData(
        reinterpret_cast<HRAWINPUT>(lParam),
        RID_INPUT,
        &stackInput,
        &size,
        sizeof(RAWINPUTHEADER));

    std::vector<BYTE> buffer;
    const RAWINPUT* rawInput = nullptr;
    if (bytesRead != static_cast<UINT>(-1))
    {
        rawInput = &stackInput;
    }
    else if (size > sizeof(stackInput))
    {
        buffer.resize(size);
        bytesRead = GetRawInputData(
            reinterpret_cast<HRAWINPUT>(lParam),
            RID_INPUT,
            buffer.data(),
            &size,
            sizeof(RAWINPUTHEADER));
        if (bytesRead == static_cast<UINT>(-1) || bytesRead == 0)
        {
            return false;
        }
        rawInput = reinterpret_cast<const RAWINPUT*>(buffer.data());
    }

    if (!rawInput)
    {
        return false;
    }

    const long long messageTimeUs = ElapsedUsFromMessageTime(static_cast<DWORD>(GetMessageTime()));

    if (rawInput->header.dwType == RIM_TYPEKEYBOARD && rawKeyboardRegistered_)
    {
        const RAWKEYBOARD& keyboard = rawInput->data.keyboard;
        DWORD virtualKey = keyboard.VKey;
        if (virtualKey == 255)
        {
            return true;
        }

        if (virtualKey == VK_SHIFT)
        {
            const UINT mapped = MapVirtualKeyW(keyboard.MakeCode, MAPVK_VSC_TO_VK_EX);
            if (mapped != 0)
            {
                virtualKey = mapped;
            }
        }

        const bool keyUp = (keyboard.Flags & RI_KEY_BREAK) != 0;
        if (ShouldIgnoreKeyboardEvent(virtualKey, keyUp ? WM_KEYUP : WM_KEYDOWN))
        {
            return true;
        }

        const DWORD identity = (virtualKey << 16) | keyboard.MakeCode | ((keyboard.Flags & RI_KEY_E0) ? 0x01000000u : 0u);
        const bool alreadyDown = rawKeyDown_.find(identity) != rawKeyDown_.end();
        if (!keyUp && alreadyDown)
        {
            return true;
        }
        if (keyUp && !alreadyDown)
        {
            return true;
        }

        if (keyUp)
        {
            rawKeyDown_.erase(identity);
        }
        else
        {
            rawKeyDown_[identity] = true;
        }

        MacroEvent event;
        event.timeUs = messageTimeUs;
        event.type = keyUp ? MacroEventType::KeyUp : MacroEventType::KeyDown;
        event.virtualKey = virtualKey;
        event.scanCode = keyboard.MakeCode;
        event.keyFlags = ((keyboard.Flags & RI_KEY_E0) != 0 || (keyboard.Flags & RI_KEY_E1) != 0) ? LLKHF_EXTENDED : 0;
        event.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        event.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        event.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        FlushPendingRelativeMouseMove(event.timeUs);
        AddEvent(event);
        return true;
    }

    if (rawInput->header.dwType != RIM_TYPEMOUSE || !rawMouseRegistered_)
    {
        return false;
    }

    const RAWMOUSE& mouse = rawInput->data.mouse;
    const long long nowUs = messageTimeUs;
    POINT cursor {};
    bool haveCursor = false;
    bool cursorVisible = false;
    bool cursorVisibilityKnown = false;
    auto ensureCursor = [&]()
    {
        if (!haveCursor)
        {
            GetCursorPos(&cursor);
            haveCursor = true;
        }
    };
    auto isCursorVisible = [&]()
    {
        if (!cursorVisibilityKnown)
        {
            cursorVisible = IsCursorShowing();
            cursorVisibilityKnown = true;
        }
        return cursorVisible;
    };

    auto addMouseButton = [&](MacroMouseButton button, bool down)
    {
        ensureCursor();
        FlushPendingRelativeMouseMove(nowUs);
        if (isCursorVisible())
        {
            AddAbsoluteMouseMove(nowUs, cursor, true);
        }
        MacroEvent event;
        event.timeUs = nowUs;
        event.type = down ? MacroEventType::MouseDown : MacroEventType::MouseUp;
        event.mouseButton = button;
        event.x = cursor.x;
        event.y = cursor.y;
        AddEvent(event);
    };

    if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0)
    {
        const LONG dx = mouse.lLastX;
        const LONG dy = mouse.lLastY;
        if (dx != 0 || dy != 0)
        {
            if (isCursorVisible())
            {
                ensureCursor();
                FlushPendingRelativeMouseMove(nowUs);
                AddAbsoluteMouseMove(nowUs, cursor, false);
            }
            else
            {
                haveLastAbsoluteMousePoint_ = false;
                AddRelativeMouseMove(nowUs, dx, dy);
            }
        }
    }
    else if (isCursorVisible())
    {
        ensureCursor();
        FlushPendingRelativeMouseMove(nowUs);
        AddAbsoluteMouseMove(nowUs, cursor, false);
    }

    const USHORT flags = mouse.usButtonFlags;
    if ((flags & RI_MOUSE_LEFT_BUTTON_DOWN) != 0) addMouseButton(MacroMouseButton::Left, true);
    if ((flags & RI_MOUSE_LEFT_BUTTON_UP) != 0) addMouseButton(MacroMouseButton::Left, false);
    if ((flags & RI_MOUSE_RIGHT_BUTTON_DOWN) != 0) addMouseButton(MacroMouseButton::Right, true);
    if ((flags & RI_MOUSE_RIGHT_BUTTON_UP) != 0) addMouseButton(MacroMouseButton::Right, false);
    if ((flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != 0) addMouseButton(MacroMouseButton::Middle, true);
    if ((flags & RI_MOUSE_MIDDLE_BUTTON_UP) != 0) addMouseButton(MacroMouseButton::Middle, false);
    if ((flags & RI_MOUSE_BUTTON_4_DOWN) != 0) addMouseButton(MacroMouseButton::X1, true);
    if ((flags & RI_MOUSE_BUTTON_4_UP) != 0) addMouseButton(MacroMouseButton::X1, false);
    if ((flags & RI_MOUSE_BUTTON_5_DOWN) != 0) addMouseButton(MacroMouseButton::X2, true);
    if ((flags & RI_MOUSE_BUTTON_5_UP) != 0) addMouseButton(MacroMouseButton::X2, false);

    if ((flags & RI_MOUSE_WHEEL) != 0 || (flags & RI_MOUSE_HWHEEL) != 0)
    {
        ensureCursor();
        FlushPendingRelativeMouseMove(nowUs);
        if (isCursorVisible())
        {
            AddAbsoluteMouseMove(nowUs, cursor, true);
        }
        MacroEvent event;
        event.timeUs = nowUs;
        event.type = MacroEventType::MouseWheel;
        event.wheelDelta = static_cast<SHORT>(mouse.usButtonData);
        event.x = cursor.x;
        event.y = cursor.y;
        AddEvent(event);
    }

    return true;
}

MacroRecorderSnapshot MacroRecorderService::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    MacroRecorderSnapshot snapshot;
    snapshot.status = status_;
    snapshot.targetWindowTitle = targetWindowTitle_;
    snapshot.targetProcessName = targetProcessName_;
    snapshot.eventCount = events_.size();
    snapshot.durationUs = status_ == MacroRecorderStatus::Recording
        ? std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startTime_).count()
        : (events_.empty() ? 0 : events_.back().timeUs);
    if (status_ == MacroRecorderStatus::Recording)
    {
        snapshot.message = L"Recording macro... press the record hotkey to stop.";
    }
    else if (status_ == MacroRecorderStatus::Stopped)
    {
        snapshot.message = L"Recording stopped. Save it when ready.";
    }
    else
    {
        snapshot.message = L"Idle.";
    }
    return snapshot;
}

bool MacroRecorderService::IsRecording() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return status_ == MacroRecorderStatus::Recording;
}

LRESULT CALLBACK MacroRecorderService::KeyboardHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (g_macroRecorder)
    {
        return g_macroRecorder->HandleKeyboard(code, wParam, lParam);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK MacroRecorderService::MouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (g_macroRecorder)
    {
        return g_macroRecorder->HandleMouse(code, wParam, lParam);
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT MacroRecorderService::HandleKeyboard(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* keyboard = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if (!keyboard || (keyboard->flags & LLKHF_INJECTED))
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (ShouldIgnoreKeyboardEvent(keyboard->vkCode, wParam))
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || wParam == WM_KEYUP || wParam == WM_SYSKEYUP)
    {
        MacroEvent event;
        event.timeUs = ElapsedUsFromMessageTime(keyboard->time);
        event.type = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? MacroEventType::KeyDown : MacroEventType::KeyUp;
        event.virtualKey = keyboard->vkCode;
        event.scanCode = keyboard->scanCode;
        event.keyFlags = keyboard->flags;
        event.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        event.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        event.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        AddEvent(event);
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT MacroRecorderService::HandleMouse(int code, WPARAM wParam, LPARAM lParam)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* mouse = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (!mouse || (mouse->flags & LLMHF_INJECTED) || mouse->dwExtraInfo == kMacroPlaybackExtraInfo)
    {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const long long nowUs = ElapsedUsFromMessageTime(mouse->time);
    if (wParam == WM_MOUSEMOVE)
    {
        if (mode_.mouseMode == MacroMouseMode::Relative && rawMouseRegistered_)
        {
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        LONG dx = 0;
        LONG dy = 0;
        if (ShouldCaptureMove(nowUs, mouse->pt, dx, dy))
        {
            MacroEvent event;
            event.timeUs = nowUs;
            event.type = MacroEventType::MouseMove;
            event.x = mouse->pt.x;
            event.y = mouse->pt.y;
            event.dx = dx;
            event.dy = dy;
            if (mode_.mouseMode == MacroMouseMode::WindowRelative)
            {
                HWND foreground = GetForegroundWindow();
                RECT rect {};
                if (foreground && GetWindowRect(foreground, &rect))
                {
                    event.x = mouse->pt.x - rect.left;
                    event.y = mouse->pt.y - rect.top;
                }
            }
            AddEvent(event);
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (IsMouseDownMessage(wParam) || IsMouseUpMessage(wParam))
    {
        MacroEvent event;
        event.timeUs = nowUs;
        event.type = IsMouseDownMessage(wParam) ? MacroEventType::MouseDown : MacroEventType::MouseUp;
        event.mouseButton = MouseButtonFromMessage(wParam, *mouse);
        event.x = mouse->pt.x;
        event.y = mouse->pt.y;
        AddEvent(event);
    }
    else if (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)
    {
        MacroEvent event;
        event.timeUs = nowUs;
        event.type = MacroEventType::MouseWheel;
        event.wheelDelta = static_cast<SHORT>(HIWORD(mouse->mouseData));
        event.x = mouse->pt.x;
        event.y = mouse->pt.y;
        AddEvent(event);
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void MacroRecorderService::AddEvent(MacroEvent event)
{
    std::lock_guard<std::mutex> lock(mutex_);
    AddEventLocked(event);
}

void MacroRecorderService::AddEventLocked(MacroEvent event)
{
    if (status_ != MacroRecorderStatus::Recording)
    {
        return;
    }
    events_.push_back(event);
    lastEventTime_ = std::chrono::steady_clock::now();
}

long long MacroRecorderService::ElapsedUs() const
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startTime_).count();
}

long long MacroRecorderService::ElapsedUsFromMessageTime(DWORD messageTime) const
{
    if (messageTime == 0)
    {
        return ElapsedUs();
    }

    const DWORD deltaMs = messageTime - startTickMs_;
    if (deltaMs > 0x7fffffffu)
    {
        return 0;
    }
    return static_cast<long long>(deltaMs) * 1000ll;
}

bool MacroRecorderService::ShouldIgnoreKeyboardEvent(DWORD virtualKey, WPARAM message) const
{
    if (!(message == WM_KEYDOWN || message == WM_SYSKEYDOWN || message == WM_KEYUP || message == WM_SYSKEYUP))
    {
        return false;
    }

    for (const MacroHotkey& hotkey : ignoredHotkeys_)
    {
        if (hotkey.virtualKey == virtualKey && CurrentModifiersMatch(hotkey.modifiers))
        {
            return true;
        }
    }
    return false;
}

bool MacroRecorderService::ShouldCaptureMove(long long nowUs, const POINT& point, LONG& dx, LONG& dy)
{
    if (!haveLastMousePoint_)
    {
        haveLastMousePoint_ = true;
        lastMousePoint_ = point;
        lastMouseMoveUs_ = nowUs;
        pendingMouseDx_ = 0;
        pendingMouseDy_ = 0;
        dx = 0;
        dy = 0;
        return mode_.mouseMode != MacroMouseMode::Relative;
    }

    dx = point.x - lastMousePoint_.x;
    dy = point.y - lastMousePoint_.y;
    if (dx == 0 && dy == 0)
    {
        return false;
    }

    if (mode_.mouseMode == MacroMouseMode::Relative)
    {
        pendingMouseDx_ += dx;
        pendingMouseDy_ += dy;
        lastMousePoint_ = point;

        const long long intervalUs = 1000000ll / std::max(30, mode_.captureRateHz);
        if (nowUs - lastMouseMoveUs_ < intervalUs)
        {
            return false;
        }

        dx = pendingMouseDx_;
        dy = pendingMouseDy_;
        pendingMouseDx_ = 0;
        pendingMouseDy_ = 0;
        lastMouseMoveUs_ = nowUs;
        return dx != 0 || dy != 0;
    }

    const long long intervalUs = 1000000ll / std::max(30, mode_.captureRateHz);
    if (nowUs - lastMouseMoveUs_ < intervalUs)
    {
        lastMousePoint_ = point;
        return false;
    }

    lastMousePoint_ = point;
    lastMouseMoveUs_ = nowUs;
    return true;
}

bool MacroRecorderService::ShouldCaptureAbsoluteMove(long long nowUs, const POINT& point, bool force)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return ShouldCaptureAbsoluteMoveLocked(nowUs, point, force);
}

bool MacroRecorderService::ShouldCaptureAbsoluteMoveLocked(long long nowUs, const POINT& point, bool force)
{
    if (!haveLastAbsoluteMousePoint_)
    {
        haveLastAbsoluteMousePoint_ = true;
        lastAbsoluteMousePoint_ = point;
        lastAbsoluteMouseMoveUs_ = nowUs;
        return true;
    }

    if (point.x == lastAbsoluteMousePoint_.x && point.y == lastAbsoluteMousePoint_.y)
    {
        return force;
    }

    if (!force)
    {
        const long long intervalUs = 1000000ll / std::max(30, mode_.captureRateHz);
        if (nowUs - lastAbsoluteMouseMoveUs_ < intervalUs)
        {
            lastAbsoluteMousePoint_ = point;
            return false;
        }
    }

    lastAbsoluteMousePoint_ = point;
    lastAbsoluteMouseMoveUs_ = nowUs;
    return true;
}

void MacroRecorderService::AddAbsoluteMouseMove(long long nowUs, const POINT& point, bool force)
{
    std::lock_guard<std::mutex> lock(mutex_);
    AddAbsoluteMouseMoveLocked(nowUs, point, force);
}

void MacroRecorderService::AddAbsoluteMouseMoveLocked(long long nowUs, const POINT& point, bool force)
{
    if (!ShouldCaptureAbsoluteMoveLocked(nowUs, point, force))
    {
        return;
    }

    MacroEvent event;
    event.timeUs = nowUs;
    event.type = MacroEventType::MouseMove;
    event.x = point.x;
    event.y = point.y;
    event.absoluteMove = true;
    AddEventLocked(event);
}

void MacroRecorderService::RecordInterceptionKeyboard(
    unsigned short code,
    unsigned short state,
    unsigned int information,
    long long timeUs)
{
    if (information == static_cast<unsigned int>(kMacroPlaybackExtraInfo))
    {
        return;
    }

    const bool keyUp = (state & kInterceptionKeyUp) != 0;
    UINT scanCodeForMapping = code;
    if ((state & kInterceptionKeyE0) != 0)
    {
        scanCodeForMapping |= 0xE000u;
    }
    else if ((state & kInterceptionKeyE1) != 0)
    {
        scanCodeForMapping |= 0xE100u;
    }

    DWORD virtualKey = MapVirtualKeyW(scanCodeForMapping, MAPVK_VSC_TO_VK_EX);
    if (virtualKey == 0)
    {
        virtualKey = MapVirtualKeyW(code, MAPVK_VSC_TO_VK);
    }
    if (virtualKey == 0)
    {
        virtualKey = code;
    }

    const WPARAM message = keyUp ? WM_KEYUP : WM_KEYDOWN;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != MacroRecorderStatus::Recording || !interceptionRecording_)
    {
        return;
    }
    if (ShouldIgnoreKeyboardEvent(virtualKey, message))
    {
        return;
    }

    const DWORD identity =
        static_cast<DWORD>(code) |
        ((state & kInterceptionKeyE0) != 0 ? 0x01000000u : 0u) |
        ((state & kInterceptionKeyE1) != 0 ? 0x02000000u : 0u);
    const bool alreadyDown = rawKeyDown_.find(identity) != rawKeyDown_.end();
    if (!keyUp && alreadyDown)
    {
        return;
    }
    if (keyUp && !alreadyDown)
    {
        return;
    }
    if (keyUp)
    {
        rawKeyDown_.erase(identity);
    }
    else
    {
        rawKeyDown_[identity] = true;
    }

    MacroEvent event;
    event.timeUs = std::max<long long>(0, timeUs);
    event.type = keyUp ? MacroEventType::KeyUp : MacroEventType::KeyDown;
    event.virtualKey = virtualKey;
    event.scanCode = code;
    event.keyFlags = ((state & kInterceptionKeyE0) != 0 || (state & kInterceptionKeyE1) != 0) ? LLKHF_EXTENDED : 0;
    event.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    event.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    event.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    AddEventLocked(event);
}

void MacroRecorderService::RecordInterceptionMouse(
    unsigned short state,
    unsigned short flags,
    short rolling,
    int x,
    int y,
    unsigned int information,
    long long timeUs)
{
    if (information == static_cast<unsigned int>(kMacroPlaybackExtraInfo))
    {
        return;
    }

    const long long eventTimeUs = std::max<long long>(0, timeUs);
    const bool moveAbsolute = (flags & kInterceptionMouseMoveAbsolute) != 0;
    const bool hasMovement = moveAbsolute || x != 0 || y != 0;
    POINT cursor {};
    bool haveCursor = false;
    auto ensureCursor = [&]()
    {
        if (!haveCursor)
        {
            GetCursorPos(&cursor);
            haveCursor = true;
        }
    };

    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ != MacroRecorderStatus::Recording || !interceptionRecording_)
    {
        return;
    }

    if (hasMovement)
    {
        if (mode_.mouseMode == MacroMouseMode::Relative && !moveAbsolute && !IsCursorShowing())
        {
            MacroEvent event;
            event.timeUs = eventTimeUs;
            event.type = MacroEventType::MouseMove;
            event.dx = x;
            event.dy = y;
            AddEventLocked(event);
        }
        else
        {
            ensureCursor();
            AddAbsoluteMouseMoveLocked(eventTimeUs, cursor, false);
        }
    }

    auto addButtonEvent = [&](MacroMouseButton button, bool down)
    {
        ensureCursor();
        MacroEvent event;
        event.timeUs = eventTimeUs;
        event.type = down ? MacroEventType::MouseDown : MacroEventType::MouseUp;
        event.mouseButton = button;
        event.x = cursor.x;
        event.y = cursor.y;
        AddEventLocked(event);
    };

    if ((state & kInterceptionMouseLeftDown) != 0) addButtonEvent(MacroMouseButton::Left, true);
    if ((state & kInterceptionMouseLeftUp) != 0) addButtonEvent(MacroMouseButton::Left, false);
    if ((state & kInterceptionMouseRightDown) != 0) addButtonEvent(MacroMouseButton::Right, true);
    if ((state & kInterceptionMouseRightUp) != 0) addButtonEvent(MacroMouseButton::Right, false);
    if ((state & kInterceptionMouseMiddleDown) != 0) addButtonEvent(MacroMouseButton::Middle, true);
    if ((state & kInterceptionMouseMiddleUp) != 0) addButtonEvent(MacroMouseButton::Middle, false);
    if ((state & kInterceptionMouseButton4Down) != 0) addButtonEvent(MacroMouseButton::X1, true);
    if ((state & kInterceptionMouseButton4Up) != 0) addButtonEvent(MacroMouseButton::X1, false);
    if ((state & kInterceptionMouseButton5Down) != 0) addButtonEvent(MacroMouseButton::X2, true);
    if ((state & kInterceptionMouseButton5Up) != 0) addButtonEvent(MacroMouseButton::X2, false);

    if ((state & kInterceptionMouseWheel) != 0)
    {
        ensureCursor();
        MacroEvent event;
        event.timeUs = eventTimeUs;
        event.type = MacroEventType::MouseWheel;
        event.wheelDelta = rolling;
        event.x = cursor.x;
        event.y = cursor.y;
        AddEventLocked(event);
    }
}

bool MacroRecorderService::RegisterRawMouseInput(HWND hwnd)
{
    if (!hwnd)
    {
        return false;
    }

    RAWINPUTDEVICE device {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
    {
        return false;
    }

    rawInputWindow_ = hwnd;
    return true;
}

bool MacroRecorderService::RegisterRawKeyboardInput(HWND hwnd)
{
    if (!hwnd)
    {
        return false;
    }

    RAWINPUTDEVICE device {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_INPUTSINK;
    device.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&device, 1, sizeof(device)))
    {
        return false;
    }

    rawInputWindow_ = hwnd;
    return true;
}

void MacroRecorderService::UnregisterRawMouseInput()
{
    if (!rawMouseRegistered_)
    {
        rawInputWindow_ = nullptr;
        return;
    }

    RAWINPUTDEVICE device {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x02;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    rawMouseRegistered_ = false;
    rawInputWindow_ = nullptr;
}

void MacroRecorderService::UnregisterRawKeyboardInput()
{
    if (!rawKeyboardRegistered_)
    {
        rawKeyDown_.clear();
        rawInputWindow_ = nullptr;
        return;
    }

    RAWINPUTDEVICE device {};
    device.usUsagePage = 0x01;
    device.usUsage = 0x06;
    device.dwFlags = RIDEV_REMOVE;
    device.hwndTarget = nullptr;
    RegisterRawInputDevices(&device, 1, sizeof(device));
    rawKeyboardRegistered_ = false;
    rawKeyDown_.clear();
    rawInputWindow_ = nullptr;
}

void MacroRecorderService::AddRelativeMouseMove(long long nowUs, LONG dx, LONG dy)
{
    if (dx == 0 && dy == 0)
    {
        return;
    }

    pendingMouseDx_ += dx;
    pendingMouseDy_ += dy;
    if (pendingMouseStartUs_ <= 0)
    {
        pendingMouseStartUs_ = nowUs;
    }

    constexpr long long kRawMouseFlushIntervalUs = 1000;
    if (nowUs - pendingMouseStartUs_ >= kRawMouseFlushIntervalUs)
    {
        FlushPendingRelativeMouseMove(nowUs);
    }
}

void MacroRecorderService::FlushPendingRelativeMouseMove(long long timeUs)
{
    if (pendingMouseDx_ == 0 && pendingMouseDy_ == 0)
    {
        pendingMouseStartUs_ = 0;
        return;
    }

    MacroEvent event;
    event.timeUs = timeUs;
    event.type = MacroEventType::MouseMove;
    event.dx = pendingMouseDx_;
    event.dy = pendingMouseDy_;
    pendingMouseDx_ = 0;
    pendingMouseDy_ = 0;
    pendingMouseStartUs_ = 0;
    lastMouseMoveUs_ = timeUs;
    AddEvent(event);
}

MacroPlaybackService::MacroPlaybackService() = default;

MacroPlaybackService::~MacroPlaybackService()
{
    Stop();
}

bool MacroPlaybackService::Start(const MacroDefinition& macro, const MacroPlaybackOptions& options, std::wstring& errorMessage)
{
    if (macro.events.empty())
    {
        errorMessage = L"This macro has no events to play.";
        return false;
    }
    if (IsPlaying())
    {
        errorMessage = L"Playback is already running.";
        return false;
    }
    if (options.requireTargetFocused && !MacroTargetWindowService::IsTargetFocused(macro))
    {
        errorMessage = L"Target window is not focused.";
        return false;
    }

    Stop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = {};
        snapshot_.status = options.startDelaySeconds > 0 ? MacroPlaybackStatus::Starting : MacroPlaybackStatus::Playing;
        snapshot_.message = options.startDelaySeconds > 0
            ? L"Playback countdown started. Focus the target window now."
            : L"Playing macro.";
        snapshot_.requestedLoops = options.loopUntilStopped ? 0 : std::max(1, options.loopCount);
        snapshot_.totalUs = macro.events.empty() ? 0 : macro.events.back().timeUs;
    }

    stopRequested_.store(false);
    playbackThread_ = std::thread(&MacroPlaybackService::PlaybackLoop, this, macro, options);
    return true;
}

void MacroPlaybackService::Stop()
{
    stopRequested_.store(true);
    if (playbackThread_.joinable())
    {
        playbackThread_.join();
    }
}

bool MacroPlaybackService::IsPlaying() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_.status == MacroPlaybackStatus::Starting || snapshot_.status == MacroPlaybackStatus::Playing;
}

MacroPlaybackSnapshot MacroPlaybackService::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool MacroPlaybackService::IsInterceptionAvailable(std::wstring& errorMessage) const
{
    InterceptionPlaybackBackend backend;
    return backend.Initialize(errorMessage);
}

void MacroPlaybackService::PlaybackLoop(MacroDefinition macro, MacroPlaybackOptions options)
{
    options.speed = std::clamp(options.speed, 0.1, 10.0);
    options.loopCount = std::clamp(options.loopCount, 1, 999);
    options.startDelaySeconds = std::clamp(options.startDelaySeconds, 0, 60);

    const bool timerResolutionRaised = timeBeginPeriod(1) == TIMERR_NOERROR;
    const HANDLE playbackThread = GetCurrentThread();
    const int previousThreadPriority = GetThreadPriority(playbackThread);
    if (previousThreadPriority != THREAD_PRIORITY_ERROR_RETURN)
    {
        SetThreadPriority(playbackThread, THREAD_PRIORITY_HIGHEST);
    }
    auto restorePlaybackTiming = [&]()
    {
        if (previousThreadPriority != THREAD_PRIORITY_ERROR_RETURN)
        {
            SetThreadPriority(playbackThread, previousThreadPriority);
        }
        if (timerResolutionRaised)
        {
            timeEndPeriod(1);
        }
    };

    if (options.startDelaySeconds > 0)
    {
        if (WaitInterruptible(std::chrono::seconds(options.startDelaySeconds)))
        {
            restorePlaybackTiming();
            return;
        }
    }

    const int maxLoops = options.loopUntilStopped ? std::numeric_limits<int>::max() : options.loopCount;
    InterceptionPlaybackBackend interceptionBackend;
    InterceptionPlaybackBackend* driverBackend = nullptr;
    if (options.backend == MacroPlaybackBackend::Interception)
    {
        std::wstring backendError;
        if (!interceptionBackend.Initialize(backendError))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.status = MacroPlaybackStatus::Failed;
            snapshot_.message = backendError.empty()
                ? L"Interception playback is unavailable."
                : backendError;
            stopRequested_.store(true);
            restorePlaybackTiming();
            return;
        }
        driverBackend = &interceptionBackend;
    }

    std::map<DWORD, MacroEvent> heldKeys;
    std::vector<MacroMouseButton> heldMouseButtons;
    auto rememberHeldInput = [&](const MacroEvent& event)
    {
        if (event.type == MacroEventType::KeyDown)
        {
            heldKeys[event.virtualKey] = event;
        }
        else if (event.type == MacroEventType::KeyUp)
        {
            heldKeys.erase(event.virtualKey);
        }
        else if (event.type == MacroEventType::MouseDown && event.mouseButton != MacroMouseButton::None)
        {
            if (std::find(heldMouseButtons.begin(), heldMouseButtons.end(), event.mouseButton) == heldMouseButtons.end())
            {
                heldMouseButtons.push_back(event.mouseButton);
            }
        }
        else if (event.type == MacroEventType::MouseUp)
        {
            heldMouseButtons.erase(
                std::remove(heldMouseButtons.begin(), heldMouseButtons.end(), event.mouseButton),
                heldMouseButtons.end());
        }
    };

    for (int loop = 1; loop <= maxLoops && !stopRequested_.load(); ++loop)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            snapshot_.status = MacroPlaybackStatus::Playing;
            snapshot_.message = options.loopUntilStopped ? L"Playing macro until stopped." : L"Playing macro.";
            snapshot_.currentLoop = loop;
        }

        const auto loopStart = std::chrono::steady_clock::now();
        for (size_t eventIndex = 0; eventIndex < macro.events.size();)
        {
            const MacroEvent& event = macro.events[eventIndex];
            const auto targetOffset = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::duration<double>(static_cast<double>(std::max<long long>(0, event.timeUs)) / 1000000.0 / options.speed));
            if (WaitUntilInterruptible(loopStart + targetOffset))
            {
                break;
            }
            if (stopRequested_.load())
            {
                break;
            }
            if (options.requireTargetFocused && !MacroTargetWindowService::IsTargetFocused(macro))
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.status = MacroPlaybackStatus::Failed;
                snapshot_.message = L"Target window lost focus. Playback stopped.";
                stopRequested_.store(true);
                break;
            }

            const bool sent = SendMacroEvent(event, macro.recordingMode.mouseMode, driverBackend);
            if (sent)
            {
                rememberHeldInput(event);
                ++eventIndex;
            }

            if (!sent)
            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.status = MacroPlaybackStatus::Failed;
                snapshot_.message = L"Windows did not accept one of the simulated inputs.";
                stopRequested_.store(true);
                break;
            }

            {
                std::lock_guard<std::mutex> lock(mutex_);
                snapshot_.elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - loopStart).count();
            }
        }
    }

    ReleaseHeldInputs(heldKeys, heldMouseButtons);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (snapshot_.status != MacroPlaybackStatus::Failed)
        {
            snapshot_.status = stopRequested_.load() ? MacroPlaybackStatus::Stopped : MacroPlaybackStatus::Complete;
            snapshot_.message = stopRequested_.load() ? L"Playback stopped." : L"Playback complete.";
        }
    }

    stopRequested_.store(true);
    restorePlaybackTiming();
}

bool MacroPlaybackService::BuildMacroInput(const MacroEvent& event, MacroMouseMode mode, INPUT& input)
{
    input = {};
    if (event.type == MacroEventType::KeyDown || event.type == MacroEventType::KeyUp)
    {
        input.type = INPUT_KEYBOARD;
        if (event.scanCode != 0)
        {
            input.ki.wScan = static_cast<WORD>(event.scanCode);
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
        }
        else
        {
            input.ki.wVk = static_cast<WORD>(event.virtualKey);
        }
        if ((event.keyFlags & LLKHF_EXTENDED) != 0)
        {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        if (event.type == MacroEventType::KeyUp)
        {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }
        input.ki.dwExtraInfo = kMacroPlaybackExtraInfo;
        return true;
    }

    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = kMacroPlaybackExtraInfo;
    if (event.type == MacroEventType::MouseMove)
    {
        if (mode == MacroMouseMode::Relative && !event.absoluteMove)
        {
            input.mi.dwFlags = MOUSEEVENTF_MOVE;
            input.mi.dx = event.dx;
            input.mi.dy = event.dy;
        }
        else
        {
            LONG x = event.x;
            LONG y = event.y;
            if (!event.absoluteMove && mode == MacroMouseMode::WindowRelative)
            {
                HWND foreground = GetForegroundWindow();
                RECT rect {};
                if (foreground && GetWindowRect(foreground, &rect))
                {
                    x += rect.left;
                    y += rect.top;
                }
            }

            const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            input.mi.dx = NormalizeAbsoluteCoordinate(x, originX, width);
            input.mi.dy = NormalizeAbsoluteCoordinate(y, originY, height);
        }
    }
    else if (event.type == MacroEventType::MouseDown || event.type == MacroEventType::MouseUp)
    {
        input.mi.dwFlags = event.type == MacroEventType::MouseDown
            ? MouseDownFlag(event.mouseButton)
            : MouseUpFlag(event.mouseButton);
        input.mi.mouseData = XButtonData(event.mouseButton);
    }
    else if (event.type == MacroEventType::MouseWheel)
    {
        input.mi.dwFlags = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(event.wheelDelta);
    }

    return input.mi.dwFlags != 0;
}

bool MacroPlaybackService::SendMacroEvent(const MacroEvent& event, MacroMouseMode mode, void* driverBackend)
{
    if (driverBackend)
    {
        return static_cast<InterceptionPlaybackBackend*>(driverBackend)->SendEvent(event, mode);
    }

    INPUT input {};
    return BuildMacroInput(event, mode, input) && SendInput(1, &input, sizeof(INPUT)) == 1;
}

void MacroPlaybackService::ReleaseHeldInputs(
    const std::map<DWORD, MacroEvent>& heldKeys,
    const std::vector<MacroMouseButton>& heldMouseButtons)
{
    std::vector<INPUT> inputs;
    inputs.reserve(heldKeys.size() + heldMouseButtons.size());

    for (auto it = heldKeys.rbegin(); it != heldKeys.rend(); ++it)
    {
        const MacroEvent& event = it->second;
        INPUT input {};
        input.type = INPUT_KEYBOARD;
        if (event.scanCode != 0)
        {
            input.ki.wScan = static_cast<WORD>(event.scanCode);
            input.ki.dwFlags = KEYEVENTF_SCANCODE;
        }
        else
        {
            input.ki.wVk = static_cast<WORD>(event.virtualKey);
        }
        if ((event.keyFlags & LLKHF_EXTENDED) != 0)
        {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        input.ki.dwFlags |= KEYEVENTF_KEYUP;
        input.ki.dwExtraInfo = kMacroPlaybackExtraInfo;
        inputs.push_back(input);
    }

    for (auto it = heldMouseButtons.rbegin(); it != heldMouseButtons.rend(); ++it)
    {
        const DWORD flag = MouseUpFlag(*it);
        if (flag == 0)
        {
            continue;
        }

        INPUT input {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flag;
        input.mi.mouseData = XButtonData(*it);
        input.mi.dwExtraInfo = kMacroPlaybackExtraInfo;
        inputs.push_back(input);
    }

    if (!inputs.empty())
    {
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    }
}

bool MacroPlaybackService::WaitInterruptible(std::chrono::steady_clock::duration duration)
{
    const auto end = std::chrono::steady_clock::now() + duration;
    return WaitUntilInterruptible(end);
}

bool MacroPlaybackService::WaitUntilInterruptible(std::chrono::steady_clock::time_point targetTime)
{
    while (!stopRequested_.load())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= targetTime)
        {
            return false;
        }
        const auto remaining = targetTime - now;
        if (remaining > std::chrono::milliseconds(3))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        else if (remaining > std::chrono::microseconds(900))
        {
            SwitchToThread();
        }
        else
        {
            YieldProcessor();
        }
    }
    return true;
}

MacroHotkeyService::~MacroHotkeyService() = default;

bool MacroHotkeyService::Register(HWND hwnd, int id, const MacroHotkey& hotkey, std::wstring& errorMessage)
{
    if (!hwnd || IsEmpty(hotkey))
    {
        return true;
    }

    Unregister(hwnd, id);
    if (!RegisterHotKey(hwnd, id, hotkey.modifiers | MOD_NOREPEAT, hotkey.virtualKey))
    {
        errorMessage = L"Could not register " + HotkeyLabel(hotkey) + L". Another app may already be using it.";
        return false;
    }

    if (std::find(registeredIds_.begin(), registeredIds_.end(), id) == registeredIds_.end())
    {
        registeredIds_.push_back(id);
    }
    return true;
}

void MacroHotkeyService::Unregister(HWND hwnd, int id)
{
    if (hwnd)
    {
        UnregisterHotKey(hwnd, id);
    }
    registeredIds_.erase(std::remove(registeredIds_.begin(), registeredIds_.end(), id), registeredIds_.end());
}

void MacroHotkeyService::UnregisterAll(HWND hwnd)
{
    for (int id : registeredIds_)
    {
        if (hwnd)
        {
            UnregisterHotKey(hwnd, id);
        }
    }
    registeredIds_.clear();
}

std::wstring MacroHotkeyService::HotkeyLabel(const MacroHotkey& hotkey)
{
    if (IsEmpty(hotkey))
    {
        return L"Unbound";
    }

    std::wstring label;
    if (hotkey.modifiers & MOD_CONTROL) label += L"Ctrl+";
    if (hotkey.modifiers & MOD_SHIFT) label += L"Shift+";
    if (hotkey.modifiers & MOD_ALT) label += L"Alt+";
    label += KeyName(hotkey.virtualKey);
    return label;
}

bool MacroHotkeyService::IsEmpty(const MacroHotkey& hotkey)
{
    return hotkey.virtualKey == 0;
}
